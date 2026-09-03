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

#ifndef XINU_RPI5_PROC_H
#define XINU_RPI5_PROC_H

#define NPROC          16      /* 8 -> 16: room for the net proc + RT tasks */
#define NULLPROC       0
#define PROC_NAME_LEN  16
#define PROC_DEFAULT_STK   4096UL

enum proc_state {
    PR_FREE = 0,   /* slot unused                                */
    PR_READY,      /* on ready list, waiting for CPU             */
    PR_CURR,       /* currpid points here                        */
    PR_WAIT,       /* blocked (proc_block) until readied          */
    PR_SLEEP,      /* timed sleep: readied by the timer tick      */
    PR_TERM        /* exited, awaiting reaper                    */
};

typedef void (*proc_entry_t)(void);

struct procent {
    enum proc_state state;
    int             prio;
    void           *stkbase;
    unsigned long   stklen;
    void           *sp;             /* saved kernel SP            */
    char            name[PROC_NAME_LEN];
    struct procent *next;           /* ready-list link            */
    unsigned long   wake_at_us;     /* PR_SLEEP: CNTPCT-us release time */
};

extern struct procent proctab[NPROC];
extern int            currpid;

void proc_init(void);
int  proc_create(proc_entry_t entry, unsigned long stksize, const char *name);
/* Heap-free variant: runs on a caller-supplied static stack (safe to call from
 * the genet_rx_tick / network-ISR context, where getmem() is not reentrant). */
int  proc_create_static(proc_entry_t entry, void *stk, unsigned long stksize,
                        const char *name);
void proc_ready(int pid);
void proc_resched(void);
void proc_yield(void);
void proc_exit(void);
int  proc_is_free(int pid);

/* Real-time additions (ported from rpi4): priority dispatch, timed sleep, and
 * timer-driven preemption.  Preemption is OFF by default (proc_set_preempt(1)
 * to enable) and suppressed inside the actor pump, so resident actors keep
 * running cooperatively; only procs that call proc_sleep_us sleep/preempt. */
void proc_block(void);                    /* PR_WAIT until proc_ready()          */
void proc_setprio(int pid, int prio);
void proc_sleep_us(unsigned long us);
void proc_timer_tick(void);               /* from the timer IRQ: wake sleepers    */
unsigned long proc_next_delay_us(void);   /* us to next RT deadline (tickless)    */
void proc_set_preempt(int on);
void proc_resched_request(void);          /* timer ISR sets the pending flag      */
void proc_preempt(void);                  /* after EOI: do the pending switch     */
void proc_actor_pump_enter(void);
void proc_actor_pump_leave(void);

/* AArch64 callee-saved (x19-x30) save / restore.  Implemented in
 * ctxsw.S.  *old_sp receives the SP value to resume `old` later;
 * new_sp is loaded immediately. */
extern void ctxsw(void **old_sp, void *new_sp);

#endif /* XINU_RPI5_PROC_H */
