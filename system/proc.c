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

/* Preemption (timer-driven round-robin).  Off by default: the cooperative
 * AIPL/actor/LLM runtime shares non-reentrant global state (the value_t heap,
 * the LLM buffers, GENET), so we only preempt when explicitly enabled around
 * isolated, self-contained processes.  The timer ISR sets g_resched_pending;
 * proc_preempt() (run after the IRQ is EOI'd) acts on it. */
static volatile int g_preempt_on;
static volatile int g_resched_pending;
static volatile unsigned long g_ctxsw;     /* context switches (live diagnostic) */
void proc_set_preempt(int on)      { g_preempt_on = on ? 1 : 0; }
void proc_resched_request(void)    { g_resched_pending = 1; }

/* Preemption gate for the cooperative actor pump.  The preemptive-networking
 * safety argument assumes the only ready-list vheap user is the app worker (so
 * a timer preempt lands only on the net process).  Resident AIPL actors break
 * that: they are vheap users sitting on the ready list, so a preempt can
 * interleave the app worker's ap_run / an actor handler with another actor and
 * race the actor scheduling (the MultiAgent wedge).  While the actor pump is
 * active we therefore suppress preemption — actors run cooperatively (the
 * proven-safe mode).  Plain compute with no actors (e.g. /llm) keeps full
 * preemption, so its network-latency win is unaffected.  Counter (not a flag)
 * so nested actor execution composes. */
static volatile int g_actor_pump;
void proc_actor_pump_enter(void) { g_actor_pump++; }
void proc_actor_pump_leave(void) { if (g_actor_pump > 0) g_actor_pump--; }

/* Live runtime accessors for the HDMI monitor (drawn by the wm in NULLPROC,
 * so they stay visible even when the app worker / HTTP path wedges). */
int           proc_preempt_on(void)  { return g_preempt_on; }
unsigned long proc_ctxsw_count(void) { return g_ctxsw; }

/* AIPL vheap mutex state (the lock/unlock impls are further down).  Declared
 * here so proc_kill() can release the lock if it reaps the process that holds
 * it — otherwise a dead owner makes every later vheap user spin-yield forever
 * (the app worker wedges; HTTP dies while ICMP survives). */
static volatile int g_aipl_owner = -1;
static volatile int g_aipl_depth;

/* Lock watchdog (diagnostic): if a spinner can't acquire after this many
 * yields the holder is wedged (never releasing), so we record the culprit and
 * force-acquire — the app worker then never sticks, keeping HTTP/diagnostics
 * alive.  Each yield normally lets the holder run to a release point, so the
 * count only climbs when the holder is blocked/dead; this won't false-trigger
 * on a legitimately long (e.g. LLM) hold. */
#define AIPL_SPIN_LIMIT 300000
volatile unsigned long g_lock_timeouts;
volatile int           g_lock_stuck_owner;        /* pid that wouldn't release */
volatile int           g_lock_stuck_owner_state;  /* its proctab state         */
volatile int           g_lock_stuck_by;           /* pid that gave up waiting   */

/* Lock state accessors for the HDMI runtime monitor (read from NULLPROC). */
int proc_aipl_owner(void) { return g_aipl_owner; }
int proc_aipl_depth(void) { return g_aipl_depth; }

static struct procent *ready_head;
static struct procent *ready_tail;

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
    struct procent *p = ready_head;
    if (p == 0) return 0;
    ready_head = p->next;
    if (ready_head == 0) ready_tail = 0;
    p->next = 0;
    return p;
}

/* Unlink `target` from the ready list if present.  A killed process MUST
 * leave the ready list: otherwise ready_pop() could later hand back a
 * PR_FREE slot, and — worse — once that slot is reused (e.g. by a new
 * actor) the stale link makes ready_push wire the node to itself, so
 * ready_pop returns the *running* process and proc_block ctxsw()'s into a
 * frame the process has already overwritten (return address = garbage). */
static void ready_remove(struct procent *target)
{
    struct procent *prev = 0, *curr = ready_head;
    while (curr) {
        if (curr == target) {
            if (prev) prev->next = curr->next;
            else      ready_head = curr->next;
            if (ready_tail == curr) ready_tail = prev;
            curr->next = 0;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
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
    return proc_create_arg(entry, stksize, name, 0);
}

int proc_create_arg(proc_entry_t entry, unsigned long stksize, const char *name, void *arg)
{
    int pid = alloc_slot();
    if (pid < 0) return -1;

    if (stksize < 1024) stksize = 1024;
    stksize = ROUNDMB(stksize);

    /* If a previous occupant of this slot died via proc_exit (self-exit),
     * its stack memory was never freed (proc_exit cannot freemem its own
     * live stack).  Free it now, before allocating the new slot's stack —
     * otherwise long-lived spawn-and-suicide patterns leak ~stksize bytes
     * per cycle and exhaust the heap.  proctab[pid].stkbase==0 after
     * proc_kill() so this is a no-op for kill-reaped slots. */
    if (proctab[pid].stkbase) {
        freemem(proctab[pid].stkbase, proctab[pid].stklen);
        proctab[pid].stkbase = 0;
        proctab[pid].stklen  = 0;
    }

    void *stk = getmem(stksize);
    if (stk == 0) return -1;

    struct procent *p = &proctab[pid];
    p->state   = PR_READY;
    p->prio    = 1;
    p->stkbase = stk;
    p->stklen  = stksize;
    p->arg     = arg;
    copy_name(p->name, name);
    p->next    = 0;

    /* Lay out an initial saved-register frame at the top of the
     * stack, in the exact order ctxsw.S restores them:
     *   [sp +   0] x29 (FP)
     *   [sp +   8] x30 (LR)   <-- where `ret` jumps; we put `entry` here
     *   [sp +  16] d14, d15   <-- AAPCS64 callee-saved FP (low 64 bits)
     *   [sp +  32] d12, d13
     *   [sp +  48] d10, d11
     *   [sp +  64] d8,  d9
     *   [sp +  80] x27, x28
     *   [sp +  96] x25, x26
     *   [sp + 112] x23, x24
     *   [sp + 128] x21, x22
     *   [sp + 144] x19, x20
     * 20 quadwords = 160 bytes, keeping the 16-byte SP alignment.
     * Initial FP regs are zeroed — fresh process has no meaningful
     * FP state. */
    unsigned long *sp_top = (unsigned long *)((unsigned char *)stk + stksize);
    unsigned long *sp     = sp_top - 20;
    sp[0]  = 0;                          /* x29 (FP)            */
    sp[1]  = (unsigned long)entry;       /* x30 (LR -> entry)   */
    sp[2]  = 0; sp[3]  = 0;              /* d14, d15            */
    sp[4]  = 0; sp[5]  = 0;              /* d12, d13            */
    sp[6]  = 0; sp[7]  = 0;              /* d10, d11            */
    sp[8]  = 0; sp[9]  = 0;              /* d8,  d9             */
    sp[10] = 0; sp[11] = 0;              /* x27, x28            */
    sp[12] = 0; sp[13] = 0;              /* x25, x26            */
    sp[14] = 0; sp[15] = 0;              /* x23, x24            */
    sp[16] = 0; sp[17] = 0;              /* x21, x22            */
    sp[18] = 0; sp[19] = 0;              /* x19, x20            */
    p->sp = (void *)sp;

    ready_push(p);
    return pid;
}

void proc_ready(int pid)
{
    if (pid <= 0 || pid >= NPROC) return;
    unsigned long d = irq_save();
    struct procent *p = &proctab[pid];
    /* Idempotent: only enqueue a process that is actually blocked.  A double
     * ready (already PR_READY = on the list, or PR_CURR = running) would
     * ready_push the node a second time, wiring its `next` into a self-loop
     * (see ready_remove's note) — ready_pop then returns a running process and
     * proc_block ctxsw()s into an overwritten frame.  Under preemption a wake
     * (ap_post) can race a preempt that already parked the target on the ready
     * list; this guard stops the resulting list corruption, which otherwise
     * drops an actor from scheduling and livelocks ap_run (the MultiAgent wedge:
     * app=WORKING, hb frozen, lock own=-1, sw still climbing). */
    if (p->state == PR_READY || p->state == PR_CURR) { irq_restore(d); return; }
    p->state = PR_READY;
    ready_push(p);
    irq_restore(d);
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
    g_ctxsw++;

    ctxsw(&oldp->sp, newp->sp);
    /* Returns here when somebody ctxsw()'s back to us. */
    irq_restore(d);
}

void proc_yield(void)
{
    proc_resched();
}

/* Timer-driven preemption point: called from irq_dispatch_c after the IRQ is
 * EOI'd (so the next process can still receive timer IRQs).  Only preempts a
 * non-NULLPROC (i.e. a real process) when enabled and a tick is pending. */
void proc_preempt(void)
{
    if (!g_preempt_on || !g_resched_pending) return;
    /* Suppress preemption while the actor pump runs (leave g_resched_pending set
     * so a tick isn't lost — the next tick after the pump leaves acts on it). */
    if (g_actor_pump) return;
    g_resched_pending = 0;
    if (currpid != NULLPROC) proc_resched();
}

/* Reap a process that is blocked (PR_WAIT) — not on the ready list and
 * not running.  Frees its stack.  Used to clean up actor processes after
 * a one-shot run so the NPROC slots aren't exhausted. */
void proc_kill(int pid)
{
    if (pid <= 0 || pid >= NPROC || pid == currpid) return;
    unsigned long d = irq_save();
    struct procent *p = &proctab[pid];
    if (p->state == PR_FREE) { irq_restore(d); return; }
    ready_remove(p);                /* never leave a freed slot on the ready list */
    /* If we're reaping a process that holds the AIPL heap lock (e.g. an actor
     * killed mid-handler by ap_killall), release it — otherwise the dead owner
     * makes every later vheap user spin-yield forever and the app worker (and
     * thus all HTTP) wedges while ICMP keeps working. */
    if (g_aipl_owner == pid) { g_aipl_owner = -1; g_aipl_depth = 0; }
    if (p->stkbase) freemem(p->stkbase, p->stklen);
    p->stkbase = 0;
    p->state   = PR_FREE;
    irq_restore(d);
}

/* Block the current process: it leaves PR_CURR for PR_WAIT (so resched
 * will not re-ready it) and we switch to the next ready process, or back
 * to NULLPROC if none is ready.  Returns once proc_ready() re-readies us
 * and the scheduler picks us again. */
void proc_block(void)
{
    unsigned long d = irq_save();
    struct procent *oldp = &proctab[currpid];
    oldp->state = PR_WAIT;

    struct procent *newp = ready_pop();
    if (newp == 0) newp = &proctab[NULLPROC];

    newp->state = PR_CURR;
    currpid     = (int)(newp - proctab);

    ctxsw(&oldp->sp, newp->sp);
    /* Resumes here when we are readied and ctxsw'd back into. */
    irq_restore(d);
}

/* ---------- AIPL vheap mutex (spin-yield) ----------
 * The AIPL runtime (cc interpreter) shares one non-reentrant value heap
 * (g_vheap in cc.c).  Under preemption, two vheap users (actors, the app
 * worker's http_build, shell cc) must never be mid-vheap-op at once.  This
 * lock serializes them: a contender spin-yields (proc_yield) so the holder
 * runs to its next release point.  It is acquired only around cc execution
 * bursts and released at every voluntary block point (ap_select), so the
 * cooperative actor pump (ap_run) still interleaves and a blocked holder
 * never wedges the heap.  The net process never takes this lock, so it can
 * always preempt a vheap user for low network latency.
 *
 * NULLPROC must not proc_block on this (proc_ready ignores pid 0), so the
 * lock spins with proc_yield — which always runs the holder — rather than
 * blocking.  Recursive (depth-counted) for safety against accidental
 * nesting; in practice depth stays 1.  (g_aipl_owner/g_aipl_depth are
 * declared near the top of this file so proc_kill can release a dead
 * owner's lock.) */

void aipl_lock(void)
{
    long spins = 0;
    for (;;) {
        unsigned long d = irq_save();
        if (g_aipl_owner < 0 || g_aipl_owner == currpid) {
            g_aipl_owner = currpid;
            g_aipl_depth++;
            irq_restore(d);
            return;
        }
        if (++spins > AIPL_SPIN_LIMIT) {
            /* Watchdog: the holder is wedged.  Record it and steal the lock so
             * the spinner (often the app worker) makes progress — keeps HTTP /
             * /lockstat alive so the culprit is visible. */
            int o = g_aipl_owner;
            g_lock_stuck_owner       = o;
            g_lock_stuck_owner_state = (o > 0 && o < NPROC) ? (int)proctab[o].state : -1;
            g_lock_stuck_by          = currpid;
            g_lock_timeouts++;
            g_aipl_owner = currpid;
            g_aipl_depth = 1;
            irq_restore(d);
            return;
        }
        irq_restore(d);
        proc_yield();                 /* let the holder run to its release point */
    }
}

void aipl_unlock(void)
{
    unsigned long d = irq_save();
    if (g_aipl_owner == currpid && --g_aipl_depth <= 0) {
        g_aipl_depth = 0;
        g_aipl_owner = -1;
    }
    irq_restore(d);
}

/* Release fully regardless of depth; returns the depth to restore later.
 * Used to drop the heap before a voluntary block (ap_select) and on the
 * let-it-crash path (where a longjmp skips the matching unlock). */
int aipl_unlock_all(void)
{
    unsigned long d = irq_save();
    int saved = 0;
    if (g_aipl_owner == currpid) { saved = g_aipl_depth; g_aipl_depth = 0; g_aipl_owner = -1; }
    irq_restore(d);
    return saved;
}

/* Reacquire to a previously-saved depth after waking from a block. */
void aipl_relock(int saved)
{
    if (saved <= 0) return;
    aipl_lock();                      /* spin-yield to depth 1 */
    unsigned long d = irq_save();
    g_aipl_depth = saved;             /* restore the full depth */
    irq_restore(d);
}

/* Unconditionally drop the lock — used by the fault handler so a process
 * that aborts while holding the heap doesn't wedge every other vheap user. */
void aipl_force_release(void)
{
    unsigned long d = irq_save();
    g_aipl_owner = -1;
    g_aipl_depth = 0;
    irq_restore(d);
}

/* Process voluntarily exits.  Marks slot free, picks next ready
 * (or NULLPROC if none), and ctxsw away — never returns. */
void proc_exit(void)
{
    irq_save();                 /* never returns -> no restore (eret in target unmasks) */
    int me = currpid;
    proctab[me].state = PR_FREE;
    ready_remove(&proctab[me]); /* in case it was (re)queued */

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
