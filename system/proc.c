// kernel/proc.c — cooperative scheduler over `proctab[]`.
//
// Pattern follows Embedded Xinu's system/resched.c / system/create.c:
//   - Single global ready list (FIFO in this Round-1 cut; Xinu uses
//     a priority queue — we can drop that in later without changing
//     callers).
//   - resched() saves the current SP into proctab[currpid].sp,
//     loads the next ready process's SP, and ctxsw()'s.
//   - create() pre-loads a fake "ctxsw save frame" on the new
//     stack so the first ctxsw INTO the new process pops it and
//     returns directly into the entry function.

#include "proc.h"
#include "memory.h"
#include "critical.h"

struct procent proctab[NPROC];
int            currpid;
extern void proc_entry_trampoline(void);   /* ctxsw.S: msr daifclr #2; br x19 */

/* Preemption (timer-driven).  OFF by default: the cooperative AIPL/actor
 * runtime shares non-reentrant state, so we only preempt when explicitly
 * enabled, and never while the actor pump runs. */
static volatile int g_preempt_on;
static volatile int g_resched_pending;
static volatile int g_actor_pump;
void proc_set_preempt(int on)      { g_preempt_on = on ? 1 : 0; }
void proc_resched_request(void)    { g_resched_pending = 1; }
void proc_actor_pump_enter(void)   { g_actor_pump++; }
void proc_actor_pump_leave(void)   { if (g_actor_pump > 0) g_actor_pump--; }

static struct procent *ready_head;
static struct procent *ready_tail;

static unsigned long proc_now_us(void)
{
    unsigned long ct, hz;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(ct));
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(hz));
    return hz ? (ct * 1000000UL) / hz : 0;
}

static void copy_name(char *dst, const char *src)
{
    int i;
    for (i = 0; i < PROC_NAME_LEN - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static void ready_push(struct procent *p)
{
    p->next = 0;
    if (ready_head == 0) {
        ready_head = ready_tail = p;
    } else {
        ready_tail->next = p;
        ready_tail = p;
    }
}

static struct procent *ready_pop(void)
{
    if (ready_head == 0) return 0;
    /* Priority-ordered dispatch (rpi3/rpi4-style): return the highest-prio ready
     * proc; ties keep FIFO order (first of the highest prio). */
    struct procent *best = ready_head, *bestprev = 0;
    struct procent *prev = ready_head, *curr = ready_head->next;
    while (curr) {
        if (curr->prio > best->prio) { best = curr; bestprev = prev; }
        prev = curr; curr = curr->next;
    }
    if (bestprev) bestprev->next = best->next;
    else          ready_head = best->next;
    if (ready_tail == best) ready_tail = bestprev;
    best->next = 0;
    return best;
}

void proc_init(void)
{
    int i;
    for (i = 0; i < NPROC; i++) {
        proctab[i].state = PR_FREE;
        proctab[i].next  = 0;
    }

    /* NULLPROC = the live boot/shell context. We don't allocate a
     * stack for it (it inherits boot.S's stack at _start) and we
     * leave .sp = 0 until the first ctxsw OUT writes the real SP. */
    struct procent *p = &proctab[NULLPROC];
    p->state   = PR_CURR;
    p->prio    = 0;
    p->stkbase = 0;
    p->stklen  = 0;
    p->sp      = 0;
    copy_name(p->name, "null/shell");

    ready_head = ready_tail = 0;
    currpid    = NULLPROC;
}

static int alloc_slot(void)
{
    int i;
    for (i = 1; i < NPROC; i++) {
        if (proctab[i].state == PR_FREE) return i;
    }
    return -1;
}

int proc_create(proc_entry_t entry, unsigned long stksize, const char *name)
{
    int pid = alloc_slot();
    if (pid < 0) return -1;

    if (stksize < 1024) stksize = 1024;
    stksize = ROUNDMB(stksize);

    void *stk = getmem(stksize);
    if (stk == 0) return -1;

    struct procent *p = &proctab[pid];
    p->state   = PR_READY;
    p->prio    = 1;
    p->stkbase = stk;
    p->stklen  = stksize;
    copy_name(p->name, name);
    p->next    = 0;

    /* Lay out an initial saved-register frame at the top of the
     * stack, in the exact order ctxsw.S restores them:
     *   [sp + 0  ] x29 (FP)
     *   [sp + 8  ] x30 (LR)   <-- where `ret` jumps; we put `entry` here
     *   [sp + 16 ] x27
     *   [sp + 24 ] x28
     *   [sp + 32 ] x25
     *   [sp + 40 ] x26
     *   ...
     *   [sp + 88 ] x20
     * 12 quadwords = 96 bytes, keeping the 16-byte SP alignment. */
    unsigned long *sp_top = (unsigned long *)((unsigned char *)stk + stksize);
    unsigned long *sp     = sp_top - 12;
    sp[0]  = 0;                          /* x29 (FP)            */
    sp[1]  = (unsigned long)proc_entry_trampoline;  /* x30 -> trampoline */
    sp[2]  = 0; sp[3]  = 0;              /* x27, x28            */
    sp[4]  = 0; sp[5]  = 0;              /* x25, x26            */
    sp[6]  = 0; sp[7]  = 0;              /* x23, x24            */
    sp[8]  = 0; sp[9]  = 0;              /* x21, x22            */
    sp[10] = (unsigned long)entry; sp[11] = 0;  /* x19 -> entry (trampoline br) */              /* x19, x20            */
    p->sp = (void *)sp;

    ready_push(p);
    return pid;
}

/* Like proc_create() but uses a caller-supplied stack buffer instead of
 * getmem().  getmem() is not reentrant against the main thread, so it is
 * unsafe to call from the genet_rx_tick / network-ISR context — which is where
 * the `cc`/`make` shell commands run (they are dispatched from the USB-keyboard
 * pump and the HTTP /run handler, both inside genet_rx_tick).  Handing in a
 * static stack keeps process creation heap-free and therefore safe there. */
int proc_create_static(proc_entry_t entry, void *stk, unsigned long stksize,
                       const char *name)
{
    int pid = alloc_slot();
    if (pid < 0) return -1;
    if (stk == 0 || stksize < 1024) return -1;

    struct procent *p = &proctab[pid];
    p->state   = PR_READY;
    p->prio    = 1;
    p->stkbase = stk;
    p->stklen  = stksize;
    copy_name(p->name, name);
    p->next    = 0;

    /* Same initial saved-register frame as proc_create() (see there). */
    unsigned long *sp_top = (unsigned long *)((unsigned char *)stk + stksize);
    sp_top = (unsigned long *)((unsigned long)sp_top & ~15UL);   /* 16-byte align */
    unsigned long *sp     = sp_top - 12;
    sp[0]  = 0;                          /* x29 (FP)            */
    sp[1]  = (unsigned long)proc_entry_trampoline;  /* x30 -> trampoline */
    sp[2]  = 0; sp[3]  = 0;
    sp[4]  = 0; sp[5]  = 0;
    sp[6]  = 0; sp[7]  = 0;
    sp[8]  = 0; sp[9]  = 0;
    sp[10] = (unsigned long)entry; sp[11] = 0;  /* x19 -> entry (trampoline br) */
    p->sp = (void *)sp;

    ready_push(p);
    return pid;
}

void proc_ready(int pid)
{
    if (pid <= 0 || pid >= NPROC) return;
    struct procent *p = &proctab[pid];
    p->state = PR_READY;
    ready_push(p);
}

/* Pick the next ready process and ctxsw into it.  Returns once we
 * resume on the original stack.  If the ready list is empty, we
 * stay where we are (the no-op makes proc_yield() safe to call
 * unconditionally). */
void proc_resched(void)
{
    unsigned long d = irq_save();
    struct procent *newp = ready_pop();
    if (newp == 0) { irq_restore(d); return; }

    int new_pid       = (int)(newp - proctab);
    struct procent *oldp = &proctab[currpid];
    int old_pid       = currpid;

    /* If the current proc is still runnable (and isn't the null
     * proc — which never goes on the ready list), park it. */
    if (oldp->state == PR_CURR && old_pid != NULLPROC) {
        oldp->state = PR_READY;
        ready_push(oldp);
    }

    newp->state = PR_CURR;
    currpid     = new_pid;

    ctxsw(&oldp->sp, newp->sp);
    /* Returns here when somebody ctxsw()'s back to us. */
    irq_restore(d);
}

/* Block the caller (PR_WAIT) until proc_ready() puts it back. */
void proc_block(void)
{
    unsigned long d = irq_save();
    struct procent *oldp = &proctab[currpid];
    oldp->state = PR_WAIT;
    struct procent *newp = ready_pop();
    if (newp == 0) newp = &proctab[NULLPROC];
    newp->state = PR_CURR;
    currpid = (int)(newp - proctab);
    ctxsw(&oldp->sp, newp->sp);
    irq_restore(d);
}

/* ---- Real-time additions (P1, ported from rpi4) ---- */
void proc_setprio(int pid, int prio)
{
    if (pid < 0 || pid >= NPROC) return;
    proctab[pid].prio = prio;
}

void proc_sleep_us(unsigned long us)
{
    extern void timer_arm_before_us(unsigned long);   /* tickless one-shot */
    unsigned long d = irq_save();
    struct procent *oldp = &proctab[currpid];
    oldp->wake_at_us = proc_now_us() + us;
    oldp->state = PR_SLEEP;
    timer_arm_before_us(us);
    struct procent *newp = ready_pop();
    if (newp == 0) newp = &proctab[NULLPROC];
    newp->state = PR_CURR;
    currpid = (int)(newp - proctab);
    ctxsw(&oldp->sp, newp->sp);
    irq_restore(d);
}

/* Called from the timer IRQ (IRQs already masked): ready any sleeper whose
 * deadline has passed and request a preemptive switch. */
void proc_timer_tick(void)
{
    unsigned long now = proc_now_us();
    int woke = 0, i;
    for (i = 0; i < NPROC; i++) {
        if (proctab[i].state == PR_SLEEP && now >= proctab[i].wake_at_us) {
            proctab[i].state = PR_READY;
            ready_push(&proctab[i]);
            woke = 1;
        }
    }
    if (woke) g_resched_pending = 1;
}

#define PROC_TICK_FLOOR_US 1000UL
#define PROC_TICK_MIN_US    200UL
unsigned long proc_next_delay_us(void)
{
    unsigned long now = proc_now_us();
    unsigned long best = PROC_TICK_FLOOR_US;
    int i;
    for (i = 0; i < NPROC; i++) {
        if (proctab[i].state == PR_SLEEP) {
            unsigned long w = proctab[i].wake_at_us;
            unsigned long dd = (w > now) ? (w - now) : 0;
            if (dd < best) best = dd;
        }
    }
    return best < PROC_TICK_MIN_US ? PROC_TICK_MIN_US : best;
}

/* Timer-driven preemption point: called after the IRQ is EOI'd. */
void proc_preempt(void)
{
    if (!g_preempt_on || !g_resched_pending) return;
    if (g_actor_pump) return;   /* actors run cooperatively */
    g_resched_pending = 0;
    if (currpid != NULLPROC) proc_resched();
}

void proc_yield(void)
{
    proc_resched();
}

/* Process voluntarily exits.  Marks slot free, picks next ready
 * (or NULLPROC if none), and ctxsw away — never returns. */
void proc_exit(void)
{
    int me = currpid;
    proctab[me].state = PR_FREE;

    struct procent *newp = ready_pop();
    if (newp == 0) newp = &proctab[NULLPROC];

    newp->state = PR_CURR;
    currpid     = (int)(newp - proctab);

    /* Throw-away storage for the saved-SP write.  Nobody will read
     * proctab[me].sp again because we're PR_FREE. */
    static void *graveyard_sp;
    ctxsw(&graveyard_sp, newp->sp);

    /* Unreachable. */
    for (;;) __asm__ volatile ("wfe");
}
