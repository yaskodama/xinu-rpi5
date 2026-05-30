// kernel/proc.h — cooperative process / scheduler interface.
//
// Mirrors classic Embedded Xinu:
//   - proctab[] holds each process's state, priority, name, stack
//     base/size, and the saved kernel SP that ctxsw() will reload.
//   - NULLPROC (pid 0) is the original boot context (kernel_main /
//     shell).  It is never placed on the ready list; instead it
//     becomes the resume target when nothing else is ready.
//   - resched() is the dispatcher; user code normally calls
//     proc_yield() (resched) or proc_exit() (resched + free slot).
//
// No preemption yet — phase S1 will add the generic timer + GIC
// IRQ that drives `resched()` from a clock tick.  For now, every
// context switch is voluntary.

#ifndef XINU_RPI4_PROC_H
#define XINU_RPI4_PROC_H

#define NPROC          256
#define NULLPROC       0
#define PROC_NAME_LEN  16
#define PROC_DEFAULT_STK   4096UL

enum proc_state {
    PR_FREE = 0,   /* slot unused                                */
    PR_READY,      /* on ready list, waiting for CPU             */
    PR_CURR,       /* currpid points here                        */
    PR_WAIT,       /* blocked (e.g. on an empty mailbox)         */
    PR_TERM        /* exited, awaiting reaper                    */
};

typedef void (*proc_entry_t)(void);

struct procent {
    enum proc_state state;
    int             prio;
    void           *stkbase;
    unsigned long   stklen;
    void           *sp;             /* saved kernel SP            */
    void           *arg;            /* opaque per-process argument */
    char            name[PROC_NAME_LEN];
    struct procent *next;           /* ready-list link            */
};

extern struct procent proctab[NPROC];
extern int            currpid;

void proc_init(void);
int  proc_create(proc_entry_t entry, unsigned long stksize, const char *name);
/* Like proc_create but stashes `arg` in proctab[pid].arg so the new
 * process can recover it (e.g. which actor it is) on first run. */
int  proc_create_arg(proc_entry_t entry, unsigned long stksize, const char *name, void *arg);
void proc_ready(int pid);
void proc_resched(void);
void proc_yield(void);
void proc_exit(void);
/* Preemptive scheduling (timer-driven round-robin).  proc_set_preempt(1)
 * enables it; proc_resched_request() is called from the timer ISR; and
 * proc_preempt() (after the IRQ is EOI'd) performs the switch. */
void proc_set_preempt(int on);
void proc_resched_request(void);
void proc_preempt(void);
/* Suppress timer preemption while the cooperative actor pump runs (actors are
 * ready-list vheap users that would otherwise race the scheduler under
 * preemption).  Counted; bracket actor execution (cc.c).  Plain non-actor
 * compute keeps full preemption. */
void proc_actor_pump_enter(void);
void proc_actor_pump_leave(void);
/* Live runtime accessors (read by the HDMI runtime monitor). */
int           proc_preempt_on(void);
unsigned long proc_ctxsw_count(void);
/* Block the current process (removes it from CURR; resched won't re-ready
 * it) until proc_ready() puts it back.  Used by mailbox receive. */
void proc_block(void);

/* AIPL vheap mutex (spin-yield): serializes the cc/vheap runtime across
 * actors / app worker / shell so preemption can't interleave two vheap ops.
 * Acquire around cc execution bursts; aipl_unlock_all/aipl_relock bracket a
 * voluntary block (ap_select) so the cooperative pump still runs. */
void aipl_lock(void);
void aipl_unlock(void);
int  aipl_unlock_all(void);   /* release fully; returns depth to restore */
void aipl_relock(int saved);  /* reacquire to a saved depth after a block */
void aipl_force_release(void);/* drop the lock unconditionally (fault/reset path) */
/* Lock state for the HDMI runtime monitor: which pid holds the vheap (-1 if
 * free) and the recursion depth.  Read from NULLPROC so it survives a wedge. */
int  proc_aipl_owner(void);
int  proc_aipl_depth(void);
/* Free a blocked/ready process's slot + stack (used to reap actor
 * processes after a one-shot run).  Must not be the current process. */
void proc_kill(int pid);

/* AArch64 callee-saved (x19-x30) save / restore.  Implemented in
 * ctxsw.S.  *old_sp receives the SP value to resume `old` later;
 * new_sp is loaded immediately. */
extern void ctxsw(void **old_sp, void *new_sp);

#endif /* XINU_RPI4_PROC_H */
