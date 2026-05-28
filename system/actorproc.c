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

#define AP_NACT  (NPROC - 1)        /* one Xinu process per actor       */
#define AP_QLEN  64

static struct {
    struct ap_msg q[AP_QLEN];
    int head, tail;
    int pid;                        /* Xinu pid, -1 = none              */
    int waiting;                    /* blocked on empty mailbox         */
} g_act[AP_NACT];

static int g_nact;
static long (*g_actor_dispatch)(long, long, long, long, long, long);

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
    for (int i = 0; i < AP_NACT; i++) {
        g_act[i].head = g_act[i].tail = 0;
        g_act[i].pid = -1;
        g_act[i].waiting = 0;
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
    int nxt = (g_act[to].tail + 1) % AP_QLEN;
    if (nxt == g_act[to].head) return;            /* full: drop */
    struct ap_msg *m = &g_act[to].q[g_act[to].tail];
    m->method = method; m->a0 = a0; m->a1 = a1; m->a2 = a2; m->a3 = a3;
    m->reply_to = reply_to;
    g_act[to].tail = nxt;
    if (g_act[to].waiting) { g_act[to].waiting = 0; proc_ready(g_act[to].pid); }
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
    while (q_empty(self)) { g_act[self].waiting = 1; proc_block(); }
    *out = g_act[self].q[g_act[self].head];
    g_act[self].head = (g_act[self].head + 1) % AP_QLEN;
}

long ap_select(long self, int n, const long *meths, struct ap_msg *out)
{
    int s = (int)self;
    for (;;) {
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
                    return meth;
                }
            }
        }
        g_act[s].waiting = 1; proc_block();        /* nothing matches yet */
    }
}

static void actor_proc_main(void)
{
    int self = (int)(long)proctab[currpid].arg;
    struct ap_msg m;
    for (;;) {
        ap_recv(self, &m);
        g_cur_reply[self] = m.reply_to;     /* read in the crash path (m may be clobbered) */
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
    if (g_nact >= AP_NACT) return -1;
    int id = g_nact;
    g_act[id].head = g_act[id].tail = 0;
    g_act[id].waiting = 0;
    g_jmp_armed[id] = 0;
    int pid = proc_create_arg(actor_proc_main, 8192, "actor", (void *)(long)id);
    if (pid < 0) return -1;
    g_act[id].pid = pid;
    g_nact++;
    return id;
}

void ap_run(void)
{
    for (int guard = 0; guard < 1000000; guard++) {
        int any_ready = 0;
        for (int i = 0; i < g_nact; i++)
            if (!g_act[i].waiting && !q_empty(i)) any_ready = 1;
        if (!any_ready) break;
        proc_yield();
    }
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
