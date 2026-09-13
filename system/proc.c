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

#ifdef SMP_SYMMETRIC
/* 対称 SMP: ready キューは 4 コアの共有物なのでロックで守る。
   ctxsw を跨いでロックを保持し、**再開した側が離す**（受け渡し方式）。
   こうしないと「ready に戻したが SP をまだ書いていないプロセス」を
   他コアが拾ってしまう。 */
#include "smpsched.h"
#include "spinlock.h"
extern struct spinlock proc_sched_lock;
#define SCHED_LOCK()    spin_lock(&proc_sched_lock)
#define SCHED_UNLOCK()  spin_unlock(&proc_sched_lock)
#define IDLE_PID()      (smp_core_id() == 0 ? NULLPROC : SMPIDLE_PID(smp_core_id()))
#define IS_IDLE(pid)    ((pid) == NULLPROC || (pid) >= SMPIDLE_BASE)
extern void proc_entry_trampoline_smp(void);
#define ENTRY_TRAMPOLINE proc_entry_trampoline_smp
#else
int            currpid;
#define SCHED_LOCK()    ((void)0)
#define SCHED_UNLOCK()  ((void)0)
#define IDLE_PID()      NULLPROC
#define IS_IDLE(pid)    ((pid) == NULLPROC)
#define ENTRY_TRAMPOLINE proc_entry_trampoline
#endif
extern void proc_entry_trampoline(void);   /* ctxsw.S: msr daifclr #2; br x19 */

/* Preemption (timer-driven).  OFF by default: the cooperative AIPL/actor
 * runtime shares non-reentrant state, so we only preempt when explicitly
 * enabled, and never while the actor pump runs. */
static volatile int g_preempt_on;
static volatile int g_resched_pending;
static volatile unsigned long g_pp_fired;   /* 先取りが実際に文脈を切り替えた回数 */
unsigned long proc_dbg_ppfired(void) { return g_pp_fired; }
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

#ifdef SMP_SYMMETRIC
/* smpsched.c から使う（どちらもロック保持が前提） */
struct procent *proc_ready_pop_locked(void)          { return ready_pop(); }
void            proc_ready_push_locked(struct procent *p) { ready_push(p); }
#endif

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
#ifdef SMP_SYMMETRIC
    smpsched_init();          /* コアごとの現在プロセスと idle を用意する */
#else
    currpid    = NULLPROC;
#endif
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
    sp[1]  = (unsigned long)ENTRY_TRAMPOLINE;  /* x30 -> trampoline */
    sp[2]  = 0; sp[3]  = 0;              /* x27, x28            */
    sp[4]  = 0; sp[5]  = 0;              /* x25, x26            */
    sp[6]  = 0; sp[7]  = 0;              /* x23, x24            */
    sp[8]  = 0; sp[9]  = 0;              /* x21, x22            */
    sp[10] = (unsigned long)entry; sp[11] = 0;  /* x19 -> entry (trampoline br) */              /* x19, x20            */
    p->sp = (void *)sp;

    SCHED_LOCK(); ready_push(p); SCHED_UNLOCK();
    return pid;
}

/* Like proc_create() but uses a caller-supplied stack buffer instead of
 * getmem().  getmem() is not reentrant against the main thread, so it is
 * unsafe to call from the genet_rx_tick / network-ISR context — which is where
 * the `cc`/`make` shell commands run (they are dispatched from the USB-keyboard
 * pump and the HTTP /run handler, both inside genet_rx_tick).  Handing in a
 * static stack keeps process creation heap-free and therefore safe there. */
static int create_static_common(proc_entry_t entry, void *stk, unsigned long stksize,
                                const char *name, int do_ready)
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
    sp[1]  = (unsigned long)ENTRY_TRAMPOLINE;  /* x30 -> trampoline */
    sp[2]  = 0; sp[3]  = 0;
    sp[4]  = 0; sp[5]  = 0;
    sp[6]  = 0; sp[7]  = 0;
    sp[8]  = 0; sp[9]  = 0;
    sp[10] = (unsigned long)entry; sp[11] = 0;  /* x19 -> entry (trampoline br) */
    p->sp = (void *)sp;

    if (do_ready) { SCHED_LOCK(); ready_push(p); SCHED_UNLOCK(); }
    else          { p->state = PR_WAIT; }
    return pid;
}

int proc_create_static(proc_entry_t entry, void *stk, unsigned long stksize,
                       const char *name)
{
    return create_static_common(entry, stk, stksize, name, 1);
}

#ifdef SMP_SYMMETRIC
/* ready にせずに作る。範囲などを設定してから proc_ready() で投入する。 */
int proc_create_static_susp(proc_entry_t entry, void *stk, unsigned long stksize,
                            const char *name)
{
    return create_static_common(entry, stk, stksize, name, 0);
}
#endif

void proc_ready(int pid)
{
    if (pid <= 0 || pid >= NPROC) return;
    struct procent *p = &proctab[pid];
    SCHED_LOCK();
    p->state = PR_READY;
    ready_push(p);
    SCHED_UNLOCK();
    __asm__ volatile("dsb sy\n\tsev" ::: "memory");   /* 待っている核を起こす */
}

/* Pick the next ready process and ctxsw into it.  Returns once we
 * resume on the original stack.  If the ready list is empty, we
 * stay where we are (the no-op makes proc_yield() safe to call
 * unconditionally). */
void proc_resched(void)
{
    unsigned long d = irq_save();
    SCHED_LOCK();
    struct procent *newp = ready_pop();
    if (newp == 0) { SCHED_UNLOCK(); irq_restore(d); return; }
#ifdef SMP_SYMMETRIC
    { extern volatile long sched_pick[SMP_NCORES]; sched_pick[smp_core_id()]++; }
#endif

    int new_pid       = (int)(newp - proctab);
    struct procent *oldp = &proctab[currpid];
    int old_pid       = currpid;

    /* If the current proc is still runnable (and isn't an idle proc —
     * those never go on the ready list), park it. */
    if (oldp->state == PR_CURR && !IS_IDLE(old_pid)) {
        oldp->state = PR_READY;
        ready_push(oldp);
    }

    newp->state = PR_CURR;
    currpid     = new_pid;

    ctxsw(&oldp->sp, newp->sp);
    /* Returns here when somebody ctxsw()'s back to us —— ロックは相手が
       握ったまま渡してくるので、こちらで離す。 */
    SCHED_UNLOCK();
    irq_restore(d);
}

/* Block the caller (PR_WAIT) until proc_ready() puts it back. */
void proc_block(void)
{
    unsigned long d = irq_save();
    SCHED_LOCK();
    struct procent *oldp = &proctab[currpid];
    oldp->state = PR_WAIT;
    struct procent *newp = ready_pop();
    if (newp == 0) newp = &proctab[IDLE_PID()];
    newp->state = PR_CURR;
    currpid = (int)(newp - proctab);
    ctxsw(&oldp->sp, newp->sp);
    SCHED_UNLOCK();
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
    SCHED_LOCK();
    struct procent *newp = ready_pop();
    if (newp == 0) newp = &proctab[IDLE_PID()];
    newp->state = PR_CURR;
    currpid = (int)(newp - proctab);
    ctxsw(&oldp->sp, newp->sp);
    SCHED_UNLOCK();
    irq_restore(d);
}

/* Called from the timer IRQ (IRQs already masked): ready any sleeper whose
 * deadline has passed and request a preemptive switch. */
void proc_timer_tick(void)
{
    unsigned long now = proc_now_us();
    int woke = 0, i;
    SCHED_LOCK();
    for (i = 0; i < NPROC; i++) {
        if (proctab[i].state == PR_SLEEP && now >= proctab[i].wake_at_us) {
            proctab[i].state = PR_READY;
            ready_push(&proctab[i]);
            woke = 1;
        }
    }
    SCHED_UNLOCK();
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
#ifdef SMP_SYMMETRIC
    /* この核が暇（idle プロセスに居る）なら、共有 ready キューから 1 本引き受ける。
     * 核0 は NULLPROC（シェル）に居るため、下の「idle なら何もしない」判定で
     * 対称スケジューラに一度も参加できず、4 コアのはずが実質 3 コアになっていた。
     * EOI 済みなのでここで文脈を切り替えてよい（通常の先取りと同じ地点）。 */
    {
        extern volatile int smpsched_on;
        extern int smpsched_poll_once(void);
        if (smpsched_on && IS_IDLE(currpid)) { smpsched_poll_once(); return; }
    }
#endif
    if (!g_preempt_on || !g_resched_pending) return;
    if (g_actor_pump) return;   /* actors run cooperatively */
    g_resched_pending = 0;
    if (!IS_IDLE(currpid)) { g_pp_fired++; proc_resched(); }
}

void proc_yield(void)
{
    proc_resched();
}

/* Process voluntarily exits.  Marks slot free, picks next ready
 * (or NULLPROC if none), and ctxsw away — never returns. */
/* そのスロットがもう空いているか。cc-run のように静的スタックを使い回す
   呼び出し元が、「ランナーが本当に消えたか」を確かめてから次を始めるため。
   proc_exit() が PR_FREE を立てたあと ctxsw で去り、以後スタックには触れない。 */
int proc_is_free(int pid)
{
    if (pid < 0 || pid >= NPROC) return 1;
    return proctab[pid].state == PR_FREE;
}

void proc_exit(void)
{
    int me = currpid;
    unsigned long d = irq_save(); (void)d;
    SCHED_LOCK();
    proctab[me].state = PR_FREE;

    struct procent *newp = ready_pop();
    if (newp == 0) newp = &proctab[IDLE_PID()];

    newp->state = PR_CURR;
    currpid     = (int)(newp - proctab);

    /* Throw-away storage for the saved-SP write.  Nobody will read
     * proctab[me].sp again because we're PR_FREE. */
    static void *graveyard_sp;
    ctxsw(&graveyard_sp, newp->sp);

    /* Unreachable. */
    for (;;) __asm__ volatile ("wfe");
}
