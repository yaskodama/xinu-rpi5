// system/actorproc.c — actors as Xinu processes, with per-actor
// mailboxes, blocking receive, and `select`.
//
// Each actor is a cooperative Xinu process (proc.c) running a receive
// loop: it blocks (proc_block) when its mailbox is empty and is woken
// (proc_ready) when a message arrives.  ap_send enqueues + wakes; because
// a handler can BLOCK mid-execution (ap_select waits for a specific
// message), this supports AIPL `select`, which the run-to-completion
// mailbox pump could not.  The behaviour is a callback (the AIPL JIT
// `dispatch`), so the same machinery drives both native demos and
// JIT-compiled AIPL actors.

#include "proc.h"
#include "uart.h"
#include "actorproc.h"
#include "critical.h"
#include "irq.h"

#define AP_NACT  (NPROC - 1)        /* one Xinu process per actor       */
#define AP_QLEN  64

static struct {
    struct ap_msg q[AP_QLEN];
    int head, tail;
    int pid;                        /* Xinu pid, -1 = none              */
    int waiting;                    /* blocked on empty mailbox         */
    unsigned int nmsg;              /* total messages handled (diagnostic) */
} g_act[AP_NACT];

static int g_nact;
static long (*g_actor_dispatch)(long, long, long, long, long, long);

/* suicide flag: set by ap_suicide(self); the receive loop checks it after
 * each dispatch returns and proc_exits the actor's process, freeing its
 * slot for ap_spawn to reuse. */
static int g_dead[AP_NACT];

/* let-it-crash: a per-actor non-local-jump buffer back to its receive loop,
 * armed only while a handler is running.  crash() (ap_crash) longjmps here. */
static void *g_actor_jmp[AP_NACT][5];   /* __builtin_setjmp frame: FP, SP, PC */
static int   g_jmp_armed[AP_NACT];
static long  g_cur_reply[AP_NACT];      /* reply_to of the handler in flight */

void ap_set_dispatch(long (*fn)(long, long, long, long, long, long)) { g_actor_dispatch = fn; }

void ap_reset(void)
{
    /* Reap any actors still live from a previous run (e.g. a resident set
     * loaded via /actor/load).  Without this, a later /compile would only
     * drop our references (pid=-1) and leak the Xinu process slots; once
     * NPROC is exhausted, ap_spawn starts returning -1, which the JIT
     * runtime then uses as a g_obj[] index — corrupting memory and wedging
     * the box.  /compile and a resident set therefore do not coexist: the
     * first /compile reaps the residents. */
    for (int i = 0; i < g_nact; i++)
        if (g_act[i].pid > 0) proc_kill(g_act[i].pid);
    aipl_force_release();   /* a fresh run starts with the heap lock clear,
                             * defending against any stale owner left behind */
    for (int i = 0; i < AP_NACT; i++) {
        g_act[i].head = g_act[i].tail = 0;
        g_act[i].pid = -1;
        g_act[i].waiting = 0;
        g_act[i].nmsg = 0;
        g_jmp_armed[i] = 0;
    }
    g_nact = 0;
}

void ap_killall(void)
{
    for (int i = 0; i < g_nact; i++)
        if (g_act[i].pid > 0) { proc_kill(g_act[i].pid); g_act[i].pid = -1; }
    g_nact = 0;
}

#define AP_REPLY (-2)                /* method id of a `now` reply        */

static int q_empty(int i) { return g_act[i].head == g_act[i].tail; }

static void ap_post(long to, long method, long reply_to, long a0, long a1, long a2, long a3)
{
    if (to < 0 || to >= g_nact) return;
    /* Enqueue + wake ATOMICALLY (IRQ-masked).  Under preemption the receiver's
     * "scan queue -> set waiting -> block" sequence (ap_recv/ap_select) runs
     * under the same mask, so a message posted here is either seen by that scan
     * (receiver doesn't block) or delivered after the receiver has parked
     * (proc_ready wakes it) — it can never fall in the gap between the scan and
     * the block.  That gap was the lost-wakeup livelock that wedged MultiAgent:
     * an Agent's reply landed between the Director's `waiting=1` and its
     * proc_block(), so the Director blocked forever on a message already in its
     * queue (Runtime monitor showed app=WORKING, hb frozen, lock own=-1, sw
     * still climbing == a livelock, not a lock deadlock). */
    unsigned long d = irq_save();
    int nxt = (g_act[to].tail + 1) % AP_QLEN;
    if (nxt == g_act[to].head) { irq_restore(d); return; }   /* full: drop */
    struct ap_msg *m = &g_act[to].q[g_act[to].tail];
    m->method = method; m->a0 = a0; m->a1 = a1; m->a2 = a2; m->a3 = a3;
    m->reply_to = reply_to;
    g_act[to].tail = nxt;
    if (g_act[to].waiting) { g_act[to].waiting = 0; proc_ready(g_act[to].pid); }
    irq_restore(d);
}

void ap_send(long to, long method, long a0, long a1, long a2, long a3)
{
    ap_post(to, method, -1, a0, a1, a2, a3);
}

long ap_call(long self, long to, long method, long a0, long a1, long a2, long a3)
{
    ap_post(to, method, self, a0, a1, a2, a3);    /* deliver, asking for a reply */
    long want[1] = { AP_REPLY };
    struct ap_msg rm;
    ap_select(self, 1, want, &rm);                /* block until the reply lands */
    return rm.a0;
}

static void ap_recv(int self, struct ap_msg *out)
{
    /* Check-and-block under IRQ mask (mirrors main.c's waiter_park): proc_block
     * resumes masked when ap_post wakes us, so the q_empty re-check can't miss a
     * message that arrived between the test and the block. */
    unsigned long d = irq_save();
    while (q_empty(self)) { g_act[self].waiting = 1; proc_block(); }
    *out = g_act[self].q[g_act[self].head];
    g_act[self].head = (g_act[self].head + 1) % AP_QLEN;
    g_act[self].nmsg++;
    irq_restore(d);
}

/* Count a message handled by a direct dispatch (cc_actor_send_*), which does
 * not go through ap_recv.  Keeps the wm window's per-actor count accurate. */
void ap_note_msg(int actor)
{
    if (actor >= 0 && actor < g_nact) g_act[actor].nmsg++;
}

long ap_select(long self, int n, const long *meths, struct ap_msg *out)
{
    int s = (int)self;
    for (;;) {
        /* Scan + set-waiting + block under IRQ mask so a matching message
         * posted concurrently (ap_post, also masked) can't be lost in the gap
         * between the scan and proc_block (the MultiAgent lost-wakeup livelock). */
        unsigned long d = irq_save();
        for (int idx = g_act[s].head; idx != g_act[s].tail; idx = (idx + 1) % AP_QLEN) {
            long meth = g_act[s].q[idx].method;
            for (int k = 0; k < n; k++) {
                if (meth == meths[k]) {
                    *out = g_act[s].q[idx];
                    int j = idx;                  /* remove at idx, compacting */
                    while (j != g_act[s].head) {
                        int p = (j - 1 + AP_QLEN) % AP_QLEN;
                        g_act[s].q[j] = g_act[s].q[p];
                        j = p;
                    }
                    g_act[s].head = (g_act[s].head + 1) % AP_QLEN;
                    irq_restore(d);
                    return meth;
                }
            }
        }
        /* Nothing matches yet.  Mark waiting and release the vheap lock (so the
         * actor that will produce the message can run), then block — all still
         * under the mask, so ap_post's enqueue+wake is serialized against this.
         * proc_block resumes masked; we then unmask and reacquire the heap. */
        g_act[s].waiting = 1;
        int held = aipl_unlock_all();
        proc_block();                              /* masked; resumes masked on wake */
        irq_restore(d);
        aipl_relock(held);
    }
}

static void actor_proc_main(void)
{
    irq_enable_all();   /* a freshly-started proc inherits the scheduler's masked
                         * DAIF from the ctxsw; restore the normal (I-unmasked) state */
    int self = (int)(long)proctab[currpid].arg;
    struct ap_msg m;
    for (;;) {
        ap_recv(self, &m);                  /* idle wait: no vheap lock held */
        g_cur_reply[self] = m.reply_to;     /* read in the crash path (m may be clobbered) */
        /* Hold the vheap lock for the duration of the handler so a preemption
         * can't interleave another vheap user (it spin-yields to us).  The
         * net process never takes the lock, so it still preempts us freely. */
        aipl_lock();
        /* Arm a non-local return so a crash() inside the handler comes back
         * here instead of taking down the process.  __builtin_setjmp returns
         * 0 on the direct path, 1 when ap_crash() longjmps. */
        g_jmp_armed[self] = 1;
        if (__builtin_setjmp(g_actor_jmp[self]) == 0) {
            long r = g_actor_dispatch ? g_actor_dispatch((long)self, m.method, m.a0, m.a1, m.a2, m.a3) : 0;
            g_jmp_armed[self] = 0;
            if (m.reply_to >= 0)            /* `now` caller is blocked for our return */
                ap_post(m.reply_to, AP_REPLY, -1, r, 0, 0, 0);
        } else {
            /* handler crashed: it was abandoned mid-flight.  Unblock a
             * synchronous caller with the crash sentinel so its supervisor
             * can retry; the process loops on to the next message. */
            int s = (int)(long)proctab[currpid].arg;
            g_jmp_armed[s] = 0;
            if (g_cur_reply[s] >= 0)
                ap_post((int)g_cur_reply[s], AP_REPLY, -1, AP_CRASH_REPLY, 0, 0, 0);
        }
        aipl_unlock_all();                  /* release fully (also covers the crash longjmp) */

        /* Suicide check: if ap_suicide(self) was called inside the handler,
         * exit the receive loop now, free our slot, and proc_exit so the
         * Xinu scheduler reclaims our stack.  ap_spawn finds pid == -1
         * slots first, so the slot is immediately reusable. */
        if (g_dead[self]) {
            g_act[self].pid = -1;
            /* leave g_dead[self] = 1 so introspection sees this slot as dead
             * until ap_spawn flips it back to 0; ap_run won't wake us either. */
            proc_exit();        /* never returns */
        }
    }
}

/* Abandon the current actor's in-flight handler (let-it-crash).  Finds the
 * actor whose process is current and, if it has an armed receive-loop frame,
 * longjmps back to it.  Called from JIT'd code via cc_crash(). */
void ap_crash(void)
{
    for (int i = 0; i < g_nact; i++)
        if (g_act[i].pid == currpid && g_jmp_armed[i])
            __builtin_longjmp(g_actor_jmp[i], 1);
    /* not inside a live actor handler — nothing to crash */
}

int ap_spawn(void)
{
    /* Find a freed slot (pid == -1, dead == 1) and reuse it; this lets
     * suicide-spawn cycles run indefinitely with a bounded actor table.
     * Without recycling, a 10 000-spawn benchmark would exhaust AP_NACT
     * even though only a few actors are alive at any moment. */
    int id = -1;
    for (int i = 0; i < g_nact; i++) {
        if (g_act[i].pid == -1) { id = i; break; }
    }
    if (id < 0) {
        if (g_nact >= AP_NACT) return -1;
        id = g_nact;
        g_nact++;
    }
    g_act[id].head = g_act[id].tail = 0;
    g_act[id].waiting = 0;
    g_act[id].nmsg = 0;
    g_jmp_armed[id] = 0;
    g_dead[id] = 0;
    int pid = proc_create_arg(actor_proc_main, 8192, "actor", (void *)(long)id);
    if (pid < 0) { g_act[id].pid = -1; return -1; }
    g_act[id].pid = pid;
    return id;
}

/* Mark an actor as dead.  The actor's own receive loop (actor_proc_main)
 * checks g_dead[self] after each dispatch returns and calls proc_exit().
 * Safe to call from inside the actor's dispatch handler on self == id. */
void ap_suicide(int id)
{
    if (id < 0 || id >= g_nact) return;
    g_dead[id] = 1;
}

/* The previous 1 000 000 guard limit was hit at the 10 M actor scale
 * (each spawn-and-suicide cycle costs a few yields, so 10 M actors with a
 * pool cap of 32 needs > 1 M yield iterations).  Bump to 200 M and add
 * a heartbeat to UART so a long run is visible in the serial console,
 * and an obvious break point on truly stuck systems. */
extern void uart_puts(const char *);
void ap_run(void)
{
    int hb_counter = 0;
    for (long guard = 0; guard < 200000000L; guard++) {
        int busy = 0;
        for (int i = 0; i < g_nact; i++) {
            if (g_act[i].pid <= 0) continue;
            /* An actor still needs the pump if it has a deliverable message, OR
             * if its process has not yet settled into the blocked wait (PR_WAIT).
             * The latter is the key: an actor that just finished a handler is
             * PR_READY (about to loop back to ap_recv) with an empty queue — the
             * old "no pending message" test returned here and LEFT it runnable
             * on the ready list.  A later preemptive /llm would then schedule
             * that stranded actor while the app worker holds the vheap lock
             * (Runtime showed app=WORKING, hb frozen, lock own=2).  Draining
             * every actor to PR_WAIT first leaves nothing runnable behind. */
            if (!g_act[i].waiting && !q_empty(i)) busy = 1;
            if (proctab[g_act[i].pid].state != PR_WAIT) busy = 1;
        }
        if (!busy) break;
        proc_yield();
        /* Heartbeat every ~1 M yields (~few seconds wall time on Pi 4).
         * Visible in serial console; useful for diagnosing whether a
         * long /actor/load is still progressing vs hung. */
        if ((++hb_counter) >= 1000000) {
            hb_counter = 0;
            uart_puts(".");      /* one dot per million yields */
        }
    }
}

/* ---- introspection for the wm "Actors" window ---- */
int ap_live_count(void) { return g_nact; }

/* Fill stats for live actor `i` (0..ap_live_count()-1): its Xinu pid, the
 * number of messages queued in its mailbox, and whether it is blocked waiting
 * on an empty mailbox.  Returns 0 on success, -1 if `i` is out of range. */
int ap_actor_stat(int i, int *pid, int *qlen, int *waiting, unsigned int *nmsg)
{
    if (i < 0 || i >= g_nact) return -1;
    if (pid)     *pid     = g_act[i].pid;
    if (qlen)    *qlen    = (g_act[i].tail - g_act[i].head + AP_QLEN) % AP_QLEN;
    if (waiting) *waiting = g_act[i].waiting;
    if (nmsg)    *nmsg    = g_act[i].nmsg;
    return 0;
}

/* ---------- native demo: two actors ping-pong via blocking receive ---------- */

static long demo_dispatch(long self, long method, long a0, long a1, long a2, long a3)
{
    (void)method; (void)a1; (void)a2; (void)a3;
    char b[2]; b[0] = (char)('0' + self); b[1] = 0;
    uart_puts("actor "); uart_puts(b); uart_puts(" hit ");
    char n[12]; int k = 0; long v = a0; if (v == 0) n[k++] = '0';
    while (v > 0) { n[k++] = (char)('0' + v % 10); v /= 10; }
    while (k--) uart_putc(n[k]);
    uart_puts("\n");
    if (a0 > 0) ap_send(1 - self, 0, a0 - 1, 0, 0, 0);
    return 0;
}

int cmd_actordemo(int argc, char **argv)
{
    int hops = 4;
    if (argc >= 2) { hops = 0; for (char *s = argv[1]; *s >= '0' && *s <= '9'; s++) hops = hops*10 + (*s - '0'); }
    ap_reset();
    ap_set_dispatch(demo_dispatch);
    int a = ap_spawn();
    int b = ap_spawn();
    if (a < 0 || b < 0) { uart_puts("actordemo: no process slots\n"); return -1; }
    uart_puts("actordemo: 2 actors as Xinu processes, ping-pong via blocking receive\n");
    ap_send(a, 0, hops, 0, 0, 0);
    ap_run();
    ap_killall();
    uart_puts("actordemo: done\n");
    return 0;
}

/* ---------- native demo: `select` consumes a named message first ---------- */

static long select_dispatch(long self, long method, long a0, long a1, long a2, long a3)
{
    (void)a0; (void)a1; (void)a2; (void)a3;
    if (method == 1) {                /* "start": select specifically for GO(3) */
        uart_puts("  start: select-ing for GO, ignoring DATA for now\n");
        long want[1] = { 3 };
        struct ap_msg m;
        ap_select(self, 1, want, &m);
        uart_puts("  -> received GO (selected past the queued DATA)\n");
    } else if (method == 2) {
        uart_puts("  DATA handled (normal FIFO)\n");
    }
    return 0;
}

int cmd_selectdemo(int argc, char **argv)
{
    (void)argc; (void)argv;
    ap_reset();
    ap_set_dispatch(select_dispatch);
    int a = ap_spawn();
    if (a < 0) { uart_puts("selectdemo: no process slot\n"); return -1; }
    uart_puts("selectdemo: mailbox order START, DATA, GO; select() takes GO before DATA\n");
    ap_send(a, 1, 0, 0, 0, 0);        /* START */
    ap_send(a, 2, 0, 0, 0, 0);        /* DATA  */
    ap_send(a, 3, 0, 0, 0, 0);        /* GO    */
    ap_run();
    ap_killall();
    uart_puts("selectdemo: done\n");
    return 0;
}

/* ---------- preemptive scheduling demo ----------
 * Two CPU-bound processes that never yield voluntarily: each spins a chunk
 * of integer work, then appends its id to a shared log, 14 times.  Under
 * cooperative scheduling one runs to completion before the other ("AAAA...
 * BBBB...").  With timer-driven preemption on, the 100 Hz tick time-slices
 * them, so the log interleaves ("ABABAB...").  Pure integer work in their own
 * stacks — no shared runtime state — so preempting them is safe. */
static char         g_plog[160];
static volatile int g_plogn;
static volatile int g_preempt_running;

static void plog_put(char c)
{
    unsigned long d = irq_save();
    if (g_plogn < (int)sizeof(g_plog) - 1) g_plog[g_plogn++] = c;
    irq_restore(d);
}

static void compute_proc_main(void)
{
    irq_enable_all();   /* unmask IRQs (see actor_proc_main) so the timer can preempt us */
    char id = (char)(long)proctab[currpid].arg;
    for (int k = 0; k < 10; k++) {
        volatile long s = 0;
        for (long i = 0; i < 3000000; i++) s += i;   /* short chunk; total demo stays short
                                                       * so the unpumped GENET RX ring survives */
        plog_put(id);
    }
    unsigned long d = irq_save(); g_preempt_running--; irq_restore(d);
    proc_exit();                                      /* never returns */
}

/* Run the demo (from the shell/NULLPROC context); copies the interleave log
 * into `out`.  Returns the log length. */
int preempt_demo(char *out, int outcap)
{
    g_plogn = 0; g_preempt_running = 2;
    int a = proc_create_arg(compute_proc_main, 4096, "cpuA", (void *)(long)'A');
    int b = proc_create_arg(compute_proc_main, 4096, "cpuB", (void *)(long)'B');
    if (a < 0 || b < 0) {
        const char *m = "preempt: no process slots"; int n = 0;
        while (m[n] && n < outcap - 1) { out[n] = m[n]; n++; } if (outcap) out[n] = 0;
        return 0;
    }
    proc_set_preempt(1);                  /* enable timer-driven preemption */
    int guard = 0;
    while (g_preempt_running > 0 && guard++ < 2000000) proc_yield();
    proc_set_preempt(0);                  /* back to cooperative */

    int n = 0;
    for (; n < g_plogn && n < outcap - 1; n++) out[n] = g_plog[n];
    if (outcap) out[n] = 0;
    return g_plogn;
}

int cmd_preempt(int argc, char **argv)
{
    (void)argc; (void)argv;
    static char o[160];
    preempt_demo(o, sizeof o);
    uart_puts("preempt: 2 CPU-bound procs, timer time-sliced -> ");
    uart_puts(o);
    uart_puts("\n  (cooperative would be AAAA...BBBB; interleaving = preemption)\n");
    return 0;
}
