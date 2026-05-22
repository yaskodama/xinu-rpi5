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

#define NPROC          8
#define NULLPROC       0
#define PROC_NAME_LEN  16
#define PROC_DEFAULT_STK   4096UL

enum proc_state {
    PR_FREE = 0,   /* slot unused                                */
    PR_READY,      /* on ready list, waiting for CPU             */
    PR_CURR,       /* currpid points here                        */
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
};

extern struct procent proctab[NPROC];
extern int            currpid;

void proc_init(void);
int  proc_create(proc_entry_t entry, unsigned long stksize, const char *name);
void proc_ready(int pid);
void proc_resched(void);
void proc_yield(void);
void proc_exit(void);

/* AArch64 callee-saved (x19-x30) save / restore.  Implemented in
 * ctxsw.S.  *old_sp receives the SP value to resume `old` later;
 * new_sp is loaded immediately. */
extern void ctxsw(void **old_sp, void *new_sp);

#endif /* XINU_RPI5_PROC_H */
