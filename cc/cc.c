// cc/cc.c — on-device C compiler driver + runtime.
//
// Pipeline: read a source file from the VFS -> cc_lex -> cc_parse ->
// cc_codegen (AArch64 machine code into an executable buffer) -> flush
// caches -> call the compiled main() in place (JIT) -> print its return
// value.  Compiled code reaches the kernel through cc_resolve_extern
// (print/putchar/puts/actor_send), which is the seam the AIPL->C output
// will use to drive the Xinu actor runtime.

#include "ccpriv.h"
#include "cc.h"
#include "vfs.h"
#include "uart.h"
#include "kmalloc.h"
#include "actor.h"
#include "actorproc.h"
#include "proc.h"        /* aipl_lock/unlock — serialize vheap under preemption */
extern void app_phase(const char *p);   /* HDMI runtime-monitor phase (tcp_server.c) */

/* ---------- arena allocator (whole-program lifetime) ---------- */

static char          *g_arena;
static unsigned long  g_acap, g_apos;
static char           g_panic[64];     /* safe sink once we OOM */

static int arena_init(unsigned long cap)
{
    g_arena = (char *)kmalloc(cap);
    g_acap  = g_arena ? cap : 0;
    g_apos  = 0;
    return g_arena != 0;
}
static void arena_free(void)
{
    if (g_arena) kfree(g_arena);
    g_arena = 0; g_acap = g_apos = 0;
}

void *cc_alloc(unsigned long n)
{
    n = (n + 15) & ~15UL;                  /* 16-byte align */
    if (!g_arena || g_apos + n > g_acap) { cc_error("compiler arena exhausted"); return g_panic; }
    void *p = g_arena + g_apos;
    g_apos += n;
    /* zero the block */
    for (unsigned long i = 0; i < n; i++) ((char *)p)[i] = 0;
    return p;
}

/* ---------- error reporting ---------- */

static int  g_err;
static char g_errbuf[128];

static void copy_msg(const char *m)
{
    int i = 0;
    while (m[i] && i < (int)sizeof(g_errbuf) - 1) { g_errbuf[i] = m[i]; i++; }
    g_errbuf[i] = 0;
}

void cc_error(const char *msg)  { if (g_err) return; g_err = 1; copy_msg(msg); }

void cc_errorc(const char *msg, char c)
{
    if (g_err) return;
    g_err = 1;
    int i = 0;
    while (msg[i] && i < (int)sizeof(g_errbuf) - 5) { g_errbuf[i] = msg[i]; i++; }
    g_errbuf[i++] = ' '; g_errbuf[i++] = '\''; g_errbuf[i++] = c; g_errbuf[i++] = '\'';
    g_errbuf[i] = 0;
}

int         cc_failed(void) { return g_err; }
const char *cc_errmsg(void) { return g_errbuf; }

/* ---------- output sink (UART, or a capture buffer for HTTP) ---------- */

static char *g_cap;            /* capture buffer, or NULL = write to UART */
static int   g_capcap, g_caplen;

static void emit_ch(char c)
{
    if (g_cap) { if (g_caplen < g_capcap - 1) g_cap[g_caplen++] = c; }
    else uart_putc(c);
}
static void emit_str(const char *s) { while (*s) emit_ch(*s++); }
static void emit_dec(long v)
{
    char buf[24]; int n = 0; unsigned long u;
    if (v < 0) { emit_ch('-'); u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (u == 0) { emit_ch('0'); return; }
    while (u > 0) { buf[n++] = (char)('0' + (u % 10)); u /= 10; }
    while (n--) emit_ch(buf[n]);
}

/* ---------- runaway-loop guard ---------- */
/* The compiler injects a call to cc_tick() at every loop back-edge.  It
 * returns 1 while time remains and 0 once a WALL-CLOCK deadline passes,
 * which makes the generated code break out of the loop.  Time-based (via
 * the generic timer) rather than an iteration count, because the MMU/
 * caches are off so per-iteration speed is unpredictable — and, crucially,
 * Historical: the line below used to say "runs inside genet_rx_tick, so a
 * runaway must abort in tens of ms or the GENET RX ring overflows".  That
 * stopped being true once /compile moved onto the dedicated app-worker
 * process and ctxsw learned to save FP across preemption (commit b160287)
 * — net stays alive even during multi-second compiles now.  Bumped from
 * 100 ms to 10 000 ms so the NQueens load-distribution benchmark (Pi
 * subtrees on the order of seconds) can run without absurd splitting. */
#define CC_TIMEOUT_MS 60000
static unsigned long g_deadline;
static int           g_aborted;

static unsigned long cc_now(void)
{
    unsigned long t;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t));
    return t;
}
static void cc_set_deadline(void)
{
    unsigned long frq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(frq));
    g_deadline = cc_now() + (frq / 1000UL) * CC_TIMEOUT_MS;
    g_aborted = 0;
}
static int cc_tick(void)
{
    if (cc_now() >= g_deadline) { g_aborted = 1; return 0; }
    return 1;
}

/* ---------- runtime builtins exposed to compiled code ---------- */

static void cc_print(long v)       { emit_dec(v); emit_ch('\n'); }
static void cc_putchar(long c)     { emit_ch((char)c); }
static void cc_puts(const char *s) { emit_str(s); emit_ch('\n'); }

static long cc_actor_send(long id, const char *method, long arg)
{
    int out = 0;
    if (actor_message((int)id, method, (int)arg, &out) != 0) return -1;
    return (long)out;
}

/* ---------- value_t runtime (for the AIPL --xinu-jit backend) ----------
 * A value_t is a tagged 64-bit word: low bit 1 => small int (value = w>>1),
 * low bit 0 => pointer to a NUL-terminated string (arena/heap data is
 * >=8-aligned, so a real string pointer never has the low bit set; 0 is
 * treated as the integer 0 / nil).  Ints are immediate (no allocation, so
 * loops are cheap); strings are pointers, and concatenation bump-allocates
 * from a small per-run heap.  The on-device compiler stays int-only — it
 * just calls these helpers, so the same compiler handles both plain C and
 * AIPL-generated value_t C. */

static char  g_vheap[32768];   /* bigger: holds chat history + LLM replies */
static int   g_vheaplen;
static void  vheap_reset(void) { g_vheaplen = 0; }
static char *vheap_alloc(int n)
{
    if (g_vheaplen + n > (int)sizeof(g_vheap)) return 0;
    char *p = &g_vheap[g_vheaplen];
    g_vheaplen += (n + 7) & ~7;           /* keep the next block 8-aligned */
    return p;
}

/* Float values are boxed (an 8-byte double in the concat heap) and the
 * pointer is tagged with bit 60 — our identity-mapped addresses fit in
 * the low 48 bits, so the tag is free and is masked off before deref. */
#define V_FLOAT_TAG  (1UL << 60)
#define V_LIST_TAG   (1UL << 61)        /* tags a pointer into the list heap */
#define V_PTR_MASK   ((1UL << 48) - 1)

static long   v_int(long n)        { return (n << 1) | 1L; }
static long   v_str(const char *p) { return (long)p; }
static int    v_is_int(long w)     { return (w & 1L) != 0; }
static int    v_is_float(long w)   { return !(w & 1L) && (((unsigned long)w >> 60) & 1UL); }
static int    v_is_list(long w)    { return !(w & 1L) && (((unsigned long)w >> 61) & 1UL); }
static int    v_is_str(long w)     { return !(w & 1L) && w != 0 && !v_is_float(w) && !v_is_list(w); }
static long   v_int_of(long w)     { return v_is_int(w) ? (w >> 1) : 0; }
static double v_to_float(long w)
{
    if (v_is_int(w))   return (double)(w >> 1);
    if (v_is_float(w)) return *(double *)((unsigned long)w & V_PTR_MASK);
    return 0.0;
}
static long v_floatval(double x)
{
    double *p = (double *)vheap_alloc(8);
    if (!p) return v_int(0);
    *p = x;
    return (long)((unsigned long)p | V_FLOAT_TAG);
}
static long v_floatlit(long bits)
{
    union { long i; double d; } u; u.i = bits;
    return v_floatval(u.d);
}

/* ---- lists / collections ----
 * A list is a pointer (tagged V_LIST_TAG) to a block in the list heap laid
 * out as [len][item0]..[item_{len-1}], each a value_t.  Like string concat,
 * lists are immutable and bump-allocated: push() copies + appends, returning
 * a new list.  Reset per run alongside the string heap. */
/* 2048 cells (16 KB) overflowed at N=6 N-Queens — Solver tree with
 * ~150 nodes each doing v_list_set(cols, r, c) ran out, after which
 * v_list_set silently returned the original ref so every child branch
 * saw the same mutated cols (sols=26 instead of 4).  32 K cells = 256 KB
 * gives margin for tree workloads up to ~N=10 (~35 000 nodes).  Also
 * UART-log the overflow so the silent-misbehave failure mode never
 * recurs unnoticed. */
static long  g_lheap[32768];
static int   g_lheaplen;
static int   g_lheap_overflows;          /* count of failed allocations */
int  cc_lheap_used(void)      { return g_lheaplen; }
int  cc_lheap_overflows(void) { return g_lheap_overflows; }
static void  lheap_reset(void) { g_lheaplen = 0; g_lheap_overflows = 0; }
static long *lheap_alloc(int ncells)
{
    if (ncells < 0 || g_lheaplen + ncells > (int)(sizeof(g_lheap)/sizeof(g_lheap[0]))) {
        if (g_lheap_overflows == 0) {                       /* one-shot log */
            extern void uart_puts(const char *);
            uart_puts("[cc] lheap OVERFLOW — array_set returns stale ref; bump g_lheap\n");
        }
        g_lheap_overflows++;
        return 0;
    }
    long *p = &g_lheap[g_lheaplen]; g_lheaplen += ncells; return p;
}
static long *v_list_ptr(long w) { return (long *)((unsigned long)w & V_PTR_MASK); }
static long  v_list_new(void)
{
    long *b = lheap_alloc(1); if (!b) return v_int(0);
    b[0] = 0;
    return (long)((unsigned long)b | V_LIST_TAG);
}
static long v_list_len(long w) { return v_int(v_is_list(w) ? v_list_ptr(w)[0] : 0); }
static long v_list_get(long w, long iv)
{
    if (!v_is_list(w)) return v_int(0);
    long *b = v_list_ptr(w); long n = b[0]; long i = v_int_of(iv);
    return (i >= 0 && i < n) ? b[1 + i] : v_int(0);
}
static long v_list_push(long w, long x)
{
    long *src = v_is_list(w) ? v_list_ptr(w) : 0;
    long n = src ? src[0] : 0;
    long *b = lheap_alloc((int)(n + 2)); if (!b) return w;   /* heap full: keep old */
    b[0] = n + 1;
    for (long i = 0; i < n; i++) b[1 + i] = src[1 + i];
    b[1 + n] = x;
    return (long)((unsigned long)b | V_LIST_TAG);
}
/* Immutable element write: return a new list whose element i is x and all
 * other elements come from `w`.  Used by AIPL `array_set(a, i, v)` —
 * matches OCaml AIPL's immutable semantics so tree-search code that
 * relies on each branch having its own state ports unchanged to Pi 4. */
static long v_list_set(long w, long iv, long x)
{
    if (!v_is_list(w)) return w;
    long *src = v_list_ptr(w);
    long n = src[0]; long i = v_int_of(iv);
    if (i < 0 || i >= n) return w;
    long *b = lheap_alloc((int)(n + 1)); if (!b) return w;
    b[0] = n;
    for (long k = 0; k < n; k++) b[1 + k] = src[1 + k];
    b[1 + i] = x;
    return (long)((unsigned long)b | V_LIST_TAG);
}
/* Allocate a fresh list of size n with every element set to v_int(0).
 * Used by AIPL `var a[N];` (fixed-size array allocation). */
static long v_list_zeros(long nv)
{
    long n = v_int_of(nv);
    if (n < 0) n = 0;
    long *b = lheap_alloc((int)(n + 1)); if (!b) return v_int(0);
    b[0] = n;
    for (long k = 0; k < n; k++) b[1 + k] = v_int(0);
    return (long)((unsigned long)b | V_LIST_TAG);
}
static int v_truthy(long w)
{
    if (w == 0) return 0;
    if (v_is_int(w))   return (w >> 1) != 0;
    if (v_is_float(w)) return v_to_float(w) != 0.0;
    if (v_is_list(w))  return v_list_ptr(w)[0] != 0;   /* non-empty */
    return ((const char *)w)[0] != 0;
}
/* Render a value to text into `buf`: strings as-is, ints/floats as decimal. */
static const char *v_render(long w, char *buf, int cap)
{
    if (v_is_list(w)) {
        long *b = v_list_ptr(w); long n = b[0];
        int i = 0;
        if (i < cap - 1) buf[i++] = '[';
        for (long k = 0; k < n && i < cap - 2; k++) {
            if (k) { if (i < cap - 1) buf[i++] = ','; if (i < cap - 1) buf[i++] = ' '; }
            char t[24];
            const char *es = v_render(b[1 + k], t, sizeof t);
            while (*es && i < cap - 2) buf[i++] = *es++;
        }
        if (i < cap - 1) buf[i++] = ']';
        buf[i] = 0;
        return buf;
    }
    if (v_is_str(w)) return (const char *)w;
    if (v_is_float(w)) {
        double d = v_to_float(w);
        int i = 0;
        if (d < 0) { buf[i++] = '-'; d = -d; }
        long ip = (long)d;
        char t[24]; int n = 0; unsigned long u = (unsigned long)ip;
        if (u == 0) t[n++] = '0';
        while (u) { t[n++] = (char)('0' + (u % 10)); u /= 10; }
        while (n && i < cap - 9) buf[i++] = t[--n];
        buf[i++] = '.';
        double frac = d - (double)ip;
        for (int k = 0; k < 6 && i < cap - 1; k++) {
            frac *= 10.0; int dg = (int)frac;
            if (dg < 0) dg = 0;
            if (dg > 9) dg = 9;
            buf[i++] = (char)('0' + dg); frac -= (double)dg;
        }
        buf[i] = 0;
        return buf;
    }
    long n = v_int_of(w);
    int i = cap - 1; buf[i--] = 0;
    int neg = 0; unsigned long u;
    if (n < 0) { neg = 1; u = (unsigned long)(-n); } else u = (unsigned long)n;
    if (u == 0) buf[i--] = '0';
    while (u && i >= 0) { buf[i--] = (char)('0' + (u % 10)); u /= 10; }
    if (neg && i >= 0) buf[i--] = '-';
    return &buf[i + 1];
}
static long v_add(long a, long b)
{
    if (v_is_str(a) || v_is_str(b)) {
        char ba[32], bb[32];
        const char *sa = v_render(a, ba, sizeof ba);
        const char *sb = v_render(b, bb, sizeof bb);
        int la = 0; while (sa[la]) la++;
        int lb = 0; while (sb[lb]) lb++;
        char *r = vheap_alloc(la + lb + 1);
        if (!r) return v_str("");
        int k = 0;
        for (int j = 0; j < la; j++) r[k++] = sa[j];
        for (int j = 0; j < lb; j++) r[k++] = sb[j];
        r[k] = 0;
        return v_str(r);
    }
    if (v_is_float(a) || v_is_float(b)) return v_floatval(v_to_float(a) + v_to_float(b));
    return v_int(v_int_of(a) + v_int_of(b));
}
static long v_sub(long a, long b) { if (v_is_float(a)||v_is_float(b)) return v_floatval(v_to_float(a)-v_to_float(b)); return v_int(v_int_of(a)-v_int_of(b)); }
static long v_mul(long a, long b) { if (v_is_float(a)||v_is_float(b)) return v_floatval(v_to_float(a)*v_to_float(b)); return v_int(v_int_of(a)*v_int_of(b)); }
static long v_div(long a, long b)
{
    if (v_is_float(a) || v_is_float(b)) { double d = v_to_float(b); return v_floatval(d != 0.0 ? v_to_float(a)/d : 0.0); }
    long d = v_int_of(b); return v_int(d ? v_int_of(a)/d : 0);
}
static long v_lt(long a, long b) { if (v_is_float(a)||v_is_float(b)) return v_int(v_to_float(a) <  v_to_float(b)); return v_int(v_int_of(a) <  v_int_of(b)); }
static long v_le(long a, long b) { if (v_is_float(a)||v_is_float(b)) return v_int(v_to_float(a) <= v_to_float(b)); return v_int(v_int_of(a) <= v_int_of(b)); }
static int  v_streq(long a, long b)
{
    const char *x = (const char *)a, *y = (const char *)b;
    if (!x || !y) return 0;
    while (*x && *y && *x == *y) { x++; y++; }
    return *x == 0 && *y == 0;
}
static long v_eq(long a, long b)
{
    if (v_is_str(a) && v_is_str(b)) return v_int(v_streq(a, b));
    if (v_is_str(a) || v_is_str(b)) return v_int(0);
    if (v_is_float(a) || v_is_float(b)) return v_int(v_to_float(a) == v_to_float(b));
    return v_int(v_int_of(a) == v_int_of(b));
}
static long v_ne(long a, long b)  { return v_int(!v_int_of(v_eq(a, b))); }
static long v_and(long a, long b) { return v_int(v_truthy(a) && v_truthy(b)); }
static long v_or(long a, long b)  { return v_int(v_truthy(a) || v_truthy(b)); }
static long v_not(long a)         { return v_int(!v_truthy(a)); }
static void v_print(long w)
{
    if (v_is_list(w)) {                 /* print arbitrarily long lists directly */
        long *p = v_list_ptr(w); long n = p[0];
        emit_ch('[');
        for (long i = 0; i < n; i++) {
            if (i) { emit_ch(','); emit_ch(' '); }
            char t[32]; emit_str(v_render(p[1 + i], t, sizeof t));
        }
        emit_ch(']'); emit_ch('\n');
        return;
    }
    char b[32]; emit_str(v_render(w, b, sizeof b)); emit_ch('\n');
}
/* v_truthy is also exported (raw 0/1) for if/while conditions. */
static long v_truthy_x(long w)    { return v_truthy(w); }

/* Higher-order list ops.  A "function value" is a small int id; the JIT
 * emits an apply(id, x) dispatcher (like the actor `dispatch`) whose address
 * the runtime registers here, so map/filter can call back into compiled AIPL
 * functions without the on-device compiler needing function pointers. */
static long (*g_apply)(long id, long x);
static void cc_set_apply(void *fn) { g_apply = (long (*)(long, long))fn; }
static long v_list_map(long w, long idv)
{
    if (!v_is_list(w) || !g_apply) return v_list_new();
    long *src = v_list_ptr(w); long n = src[0]; long id = v_int_of(idv);
    long out = v_list_new();
    for (long i = 0; i < n; i++) out = v_list_push(out, g_apply(id, v_list_ptr(w)[1 + i]));
    return out;
}
static long v_list_filter(long w, long idv)
{
    if (!v_is_list(w) || !g_apply) return v_list_new();
    long *src = v_list_ptr(w); long n = src[0]; long id = v_int_of(idv);
    long out = v_list_new();
    for (long i = 0; i < n; i++) {
        long e = v_list_ptr(w)[1 + i];
        if (v_truthy(g_apply(id, e))) out = v_list_push(out, e);
    }
    return out;
}

/* ---------- asynchronous mailbox (cooperative message pump) ----------
 * `send obj.m(args)` in AIPL is fire-and-forget: it enqueues a message
 * and returns immediately.  After main() (and after each /actor/send) we
 * run cc_pump(), which dequeues in FIFO order and dispatches each — a
 * handler's own `send`s are enqueued and processed in turn.  This gives
 * real concurrent-actor behaviour without threads.  `now` stays a direct
 * synchronous dispatch (it needs the reply).  The pump is time-bounded
 * (shares the runaway deadline) so an endless send cascade can't hang. */

/* The current program's dispatch (set before running main / a message);
 * the actor processes invoke it for each delivered message. */
static long (*g_active_dispatch)(long, long, long, long, long, long);

/* `send obj.m(args)` -> enqueue on the target actor's mailbox (a Xinu
 * process); the cooperative scheduler delivers it. */
static void cc_enqueue(long to, long mid, long a0, long a1, long a2, long a3)
{
    ap_send(to, mid, a0, a1, a2, a3);
}

/* `new C()` -> spawn an actor process; returns its id (== g_obj index). */
static long cc_actor_new(void) { return (long)ap_spawn(); }

/* `suicide()` -> mark `self` as dead.  Receive loop in actorproc.c sees the
 * flag after this dispatch returns, releases the vheap lock, then proc_exits
 * so the slot is reusable by a subsequent ap_spawn.  The JIT passes `self`
 * (the int actor id) as the first argument; we strip the value_t tag so the
 * runtime gets a plain int. */
extern void ap_suicide(int id);
static long cc_actor_suicide(long self_v)
{
    ap_suicide((int)v_int_of(self_v));
    return v_int(0);
}

/* Window-layout builtins (device/video/wm.c) — let an AIPL "Layout" actor
 * reposition the on-screen windows, driven by the Py-I screen designer.
 * Args arrive as value_t ints; idx is the window's add order (0-based). */
extern int wm_window_count(void);
extern int wm_window_move(int idx, int x, int y);
extern int wm_window_resize(int idx, int w, int h);
extern int wm_window_font(int idx, int scale);
static long cc_win_move(long id, long x, long y)
{
    wm_window_move((int)v_int_of(id), (int)v_int_of(x), (int)v_int_of(y));
    return v_int(0);
}
static long cc_win_resize(long id, long w, long h)
{
    wm_window_resize((int)v_int_of(id), (int)v_int_of(w), (int)v_int_of(h));
    return v_int(0);
}
static long cc_win_font(long id, long scale)
{
    wm_window_font((int)v_int_of(id), (int)v_int_of(scale));
    return v_int(0);
}
static long cc_win_count(void) { return v_int(wm_window_count()); }

/* Actor graphics (device/video/video.c) — draw into the Graphics window. */
extern void gfx_clear(void);
extern void gfx_line(int x0, int y0, int x1, int y1, int color);
extern void gfx_circle(int cx, int cy, int r, int color);
extern void gfx_glass(int ax, int ay, int az);
static long cc_gfx_clear(void) { gfx_clear(); return v_int(0); }
static long cc_gfx_glass(long ax, long ay, long az)
{
    gfx_glass((int)v_int_of(ax), (int)v_int_of(ay), (int)v_int_of(az));
    return v_int(0);
}
static long cc_gfx_line(long x0, long y0, long x1, long y1, long col)
{
    gfx_line((int)v_int_of(x0), (int)v_int_of(y0), (int)v_int_of(x1),
             (int)v_int_of(y1), (int)v_int_of(col));
    return v_int(0);
}
static long cc_gfx_circle(long cx, long cy, long r, long col)
{
    gfx_circle((int)v_int_of(cx), (int)v_int_of(cy), (int)v_int_of(r),
               (int)v_int_of(col));
    return v_int(0);
}

/* `now obj.m(args)` inside a method -> synchronous call: block this actor
 * until the target replies with its return value. */
static long cc_call(long self, long to, long method, long a0, long a1, long a2, long a3)
{
    return ap_call(self, to, method, a0, a1, a2, a3);
}

/* `select { case m(v): ... }` runtime: block until one of m0..m{n-1}
 * arrives, stash its args for cc_sel_arg, and return the matched method. */
static long g_sel[4];
static long cc_select(long self, long n, long m0, long m1, long m2, long m3)
{
    long meths[4] = { m0, m1, m2, m3 };
    struct ap_msg m;
    long meth = ap_select(self, (int)n, meths, &m);
    g_sel[0] = m.a0; g_sel[1] = m.a1; g_sel[2] = m.a2; g_sel[3] = m.a3;
    return meth;
}
static long cc_sel_arg(long i) { return (i >= 0 && i < 4) ? g_sel[i] : v_int(0); }

/* `saga { step {..} compensate {..} ... }` runtime.  AIPL signals a step
 * failure by raising; this int-only backend has no exceptions, so the AIPL
 * step body calls `fail()` instead.  The translator emits a driver that runs
 * the bodies in order, stops at the first fail(), and then runs the already-
 * completed steps' compensate blocks in reverse (LIFO).  One flag, reset at
 * each saga; nested sagas are not supported. */
static int  g_saga_failed;
static void cc_saga_reset(void)  { g_saga_failed = 0; }
static long cc_saga_fail(void)   { g_saga_failed = 1; return v_int(0); }
static long cc_saga_failed(void) { return g_saga_failed; }   /* plain 0/1 for raw C */

/* let-it-crash: an actor handler calls crash() to abandon itself; ap_crash
 * longjmps it back to its receive loop (does not return).  A supervisor's
 * synchronous `now` then returns the crash sentinel, which crashed() yields
 * so the AIPL code can compare `r == crashed()`. */
static long cc_crash(void)         { ap_crash(); return v_int(0); }
static long cc_crashed_value(void) { return AP_CRASH_REPLY; }

/* AIPL builtin llm(prompt): run the baked LLM on `prompt` (a value_t string)
 * and return the generated text as a value_t string (in the concat heap, so it
 * persists / can be stored in a field).  Lets an AIPL actor talk to the LLM. */
extern int  llm_run(const char *prompt, int max_new, char *out, int outcap, int echo);
extern int  llm_chat(const char *msg, int max_new, char *out, int outcap);
extern void llm_session_reset(void);
static long cc_llm(long prompt)          /* one-shot: re-encode `prompt` each call */
{
    char pbuf[1024];
    const char *src = v_is_str(prompt) ? (const char *)prompt
                                       : v_render(prompt, pbuf, sizeof pbuf);
    char *out = vheap_alloc(420);
    if (!out) return v_str("");
    llm_run(src, 32, out, 420, 0);       /* 32 new tokens, no prompt echo */
    cc_set_deadline();                   /* LLM calls are legitimately slow: restart the
                                          * runaway timer so a caller's loop isn't aborted */
    return v_str(out);
}
static long cc_chat(long msg)            /* stateful: continues the KV-cache session */
{
    char pbuf[1024];
    const char *src = v_is_str(msg) ? (const char *)msg
                                    : v_render(msg, pbuf, sizeof pbuf);
    char *out = vheap_alloc(420);
    if (!out) return v_str("");
    llm_chat(src, 32, out, 420);
    cc_set_deadline();                   /* restart the runaway timer after the slow call */
    return v_str(out);
}

unsigned long cc_resolve_extern(const char *name)
{
    struct { const char *n; void *f; } tab[] = {
        { "print",      (void *)&cc_print     },
        { "putchar",    (void *)&cc_putchar   },
        { "putc",       (void *)&cc_putchar   },
        { "puts",       (void *)&cc_puts      },
        { "actor_send", (void *)&cc_actor_send},
        { "__cc_tick",  (void *)&cc_tick      },
        /* value_t runtime for the AIPL --xinu-jit backend */
        { "v_int",      (void *)&v_int        },
        { "v_str",      (void *)&v_str        },
        { "v_floatlit", (void *)&v_floatlit   },
        { "v_add",      (void *)&v_add        },
        { "v_sub",      (void *)&v_sub        },
        { "v_mul",      (void *)&v_mul        },
        { "v_div",      (void *)&v_div        },
        { "v_lt",       (void *)&v_lt         },
        { "v_le",       (void *)&v_le         },
        { "v_eq",       (void *)&v_eq         },
        { "v_ne",       (void *)&v_ne         },
        { "v_and",      (void *)&v_and        },
        { "v_or",       (void *)&v_or         },
        { "v_not",      (void *)&v_not        },
        { "v_print",    (void *)&v_print      },
        { "v_truthy",   (void *)&v_truthy_x   },
        { "v_int_of",   (void *)&v_int_of     },
        { "enqueue",    (void *)&cc_enqueue   },
        { "cc_actor_new",(void *)&cc_actor_new},
        { "cc_actor_suicide", (void *)&cc_actor_suicide},
        { "cc_win_move",  (void *)&cc_win_move  },
        { "cc_win_resize",(void *)&cc_win_resize},
        { "cc_win_font",  (void *)&cc_win_font  },
        { "cc_win_count", (void *)&cc_win_count },
        { "cc_gfx_clear", (void *)&cc_gfx_clear },
        { "cc_gfx_glass", (void *)&cc_gfx_glass },
        { "cc_gfx_line",  (void *)&cc_gfx_line  },
        { "cc_gfx_circle",(void *)&cc_gfx_circle},
        { "cc_call",    (void *)&cc_call      },
        { "cc_select",  (void *)&cc_select    },
        { "cc_sel_arg", (void *)&cc_sel_arg   },
        { "cc_saga_reset",  (void *)&cc_saga_reset  },
        { "cc_saga_fail",   (void *)&cc_saga_fail   },
        { "cc_saga_failed", (void *)&cc_saga_failed },
        { "cc_crash",         (void *)&cc_crash         },
        { "cc_crashed_value", (void *)&cc_crashed_value },
        { "cc_llm",       (void *)&cc_llm       },
        { "cc_chat",      (void *)&cc_chat      },
        { "v_list_new",    (void *)&v_list_new    },
        { "v_list_push",   (void *)&v_list_push   },
        { "v_list_set",    (void *)&v_list_set    },
        { "v_list_get",    (void *)&v_list_get    },
        { "v_list_len",    (void *)&v_list_len    },
        { "v_list_zeros",  (void *)&v_list_zeros  },
        { "v_list_map",    (void *)&v_list_map    },
        { "v_list_filter", (void *)&v_list_filter },
        { 0, 0 }
    };
    for (int i = 0; tab[i].n; i++) {
        const char *x = tab[i].n, *y = name;
        while (*x && *y && *x == *y) { x++; y++; }
        if (*x == 0 && *y == 0) return (unsigned long)tab[i].f;
    }
    return 0;
}

/* ---------- I-cache / D-cache sync for JIT ---------- */

static void cc_sync_icache(void *p, unsigned long len)
{
    unsigned long start = (unsigned long)p;
    unsigned long end   = start + len;
    /* Self-modifying-code sync for Cortex-A72 on this XINU port — note that
     * SCTLR_EL1.C is left OFF (D-cache disabled at EL1) while SCTLR_EL1.I
     * is ON (I-cache enabled).  In that asymmetric configuration:
     *   * JIT stores skip the L1 D-cache entirely (going straight to RAM
     *     via the store buffer), so `dc cvau` (clean to PoU) is a no-op
     *     for the data we just wrote.
     *   * The unified L2 may still hold STALE instruction lines that L1 I
     *     loaded on a previous call — those have to be invalidated all the
     *     way to PoC (Point of Coherency), not just PoU.
     * Replay both the canonical "JIT write" sequence (dc cvau + ic ivau, by
     * VA, to PoU) AND a `dc civac` (clean+invalidate by VA to PoC) so any
     * stale L2 line is evicted regardless of which path it came in on.
     * Then a final `ic iallu` + dsb sy + isb makes sure the next fetch can
     * only come from RAM, where codegen just wrote the new bytes. */
    for (unsigned long a = start; a < end; a += 64) {           /* L2 line = 64 B */
        __asm__ volatile ("dc cvau,  %0" :: "r"(a) : "memory");
        __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile ("dsb sy" ::: "memory");
    for (unsigned long a = start; a < end; a += 64)
        __asm__ volatile ("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile ("ic iallu" ::: "memory");                 /* belt + braces */
    __asm__ volatile ("dsb sy" ::: "memory");
    __asm__ volatile ("isb"    ::: "memory");
    (void)end;
}

/* ---------- the `cc` shell command ---------- */

/* Parse arena and codegen output buffer sizes.
 *
 * CC_ARENA — temporary, freed after parse+codegen.  Each AST node is
 * ~64 bytes, each func_t + locals ~200 bytes.  A 17-function AIPL
 * tree-decomposition program (e.g. parallel N-Queens) needs $\sim$ 500 KB
 * easily, so 256 KB was a hard ceiling that produced ``cc: compile
 * error'' (arena exhausted) silently.  Bumped to 2 MB to comfortably
 * handle generated dispatch tables for actor systems up to ~50 methods.
 *
 * CC_CODECAP — AArch64 native output; bumped to 256 KB.  At 50 bytes per
 * instruction and a few hundred instructions per method, 17 methods +
 * dispatch fit easily. */
#define CC_ARENA   (2  * 1024 * 1024)
#define CC_CODECAP (256 * 1024)

/* Shared core: compile `src` (n bytes) and execute in place.  Program
 * output goes wherever the sink (g_cap / UART) currently points.
 * Returns 0 (sets *retval), -1 on compile error (g_errbuf set), -2 OOM. */
static int compile_run_core(const char *src, unsigned long n, long *retval)
{
    if (!arena_init(CC_ARENA)) return -2;
    g_err = 0; g_errbuf[0] = 0;

    char *s = (char *)cc_alloc(n + 1);
    for (unsigned long i = 0; i < n; i++) s[i] = src[i];
    s[n] = 0;

    token_t *toks = cc_lex(s);
    func_t  *fns  = cc_failed() ? 0 : cc_parse(toks);

    unsigned char *code = 0;
    int entry = 0, len = -1;
    if (!cc_failed()) {
        code = (unsigned char *)kmalloc(CC_CODECAP);
        if (!code) { arena_free(); return -2; }
        len = cc_codegen(fns, code, CC_CODECAP, &entry);
    }
    if (cc_failed() || len < 0) { if (code) kfree(code); arena_free(); return -1; }

    int doff = cc_func_offset("dispatch");
    int aoff = cc_func_offset("apply");          /* map/filter callback dispatcher */
    cc_sync_icache(code, (unsigned long)len);
    cc_set_deadline();
    vheap_reset();              /* fresh string-concat heap for this run */
    lheap_reset();              /* and a fresh list heap */
    ap_reset();                 /* fresh actor set */
    g_active_dispatch = (doff >= 0) ? (void *)(code + doff) : 0;
    ap_set_dispatch(g_active_dispatch);
    cc_set_apply((aoff >= 0) ? (void *)(code + aoff) : 0);
    long (*entryfn)(void) = (long (*)(void))(code + entry);
    proc_actor_pump_enter();    /* actors run cooperatively (no timer preempt) */
    app_phase("co-main");
    aipl_lock();                /* hold the vheap across main()'s direct vheap use */
    long rc = entryfn();        /* main(): spawn actor processes + send */
    aipl_unlock();              /* release before ap_run (actors lock individually) */
    app_phase("co-run");
    ap_run();                   /* drive actor processes until quiescent */
    ap_killall();               /* one-shot: reap the actor processes */
    proc_actor_pump_leave();
    g_active_dispatch = 0;
    if (retval) *retval = rc;

    kfree(code);
    arena_free();
    return 0;
}

int cc_run_source(const char *src, int srclen, char *out, int outcap, long *retval)
{
    g_cap = out; g_capcap = outcap; g_caplen = 0;
    if (out && outcap > 0) out[0] = 0;

    long rv = 0;
    int rc = compile_run_core(src, (unsigned long)(srclen < 0 ? 0 : srclen), &rv);

    if (rc == 0 && g_aborted && g_cap) {
        const char *note = "cc: aborted (ran past the 100ms runaway-loop deadline)\n";
        for (int i = 0; note[i] && g_caplen < g_capcap - 1; i++) g_cap[g_caplen++] = note[i];
    }
    if (g_cap) { g_cap[(g_caplen < g_capcap) ? g_caplen : (g_capcap - 1)] = 0; }
    g_cap = 0;

    if (rc == 0) { if (retval) *retval = rv; return 0; }

    /* compile error / OOM: report into `out` */
    if (out && outcap > 0) {
        int p = 0;
        const char *pfx = "cc: ";
        while (*pfx && p < outcap - 1) out[p++] = *pfx++;
        const char *e = g_errbuf[0] ? g_errbuf : (rc == -2 ? "out of memory" : "compile error");
        while (*e && p < outcap - 1) out[p++] = *e++;
        out[p] = 0;
    }
    return rc;
}

#include "qemu_repro_src.h"   /* TEMPORARY: QEMU repro of the actor+select wedge */
int cmd_repro(int argc, char **argv)
{
    (void)argc; (void)argv;
    static char out[2048];
    long rv = 0;
    uart_puts("repro: BEGIN cc_run_source(SelMin)\n");
    int rc = cc_run_source(SELMIN_SRC, (int)sizeof(SELMIN_SRC) - 1, out, sizeof out, &rv);
    uart_puts("repro: cc_run_source RETURNED\n");
    uart_puts(out);
    uart_puts(rc == 0 ? "\nrepro: OK\n" : "\nrepro: ERR\n");
    return 0;
}

int cmd_cc(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: cc <file.c>\n"); return -1; }

    vfs_node_t *f = vfs_resolve(argv[1]);
    if (!f || f->kind != VFS_FILE) {
        uart_puts("cc: no such file: "); uart_puts(argv[1]); uart_puts("\n");
        return -1;
    }

    static char out[4096];
    long rv = 0;
    int rc = cc_run_source((const char *)f->data, (int)f->size, out, sizeof out, &rv);
    uart_puts(out);
    if (rc == 0) { uart_puts("=> "); emit_dec(rv); uart_puts("\n"); }
    else         { uart_puts("\n"); }
    return rc == 0 ? 0 : -1;
}

/* ============================================================
 *  Resident actor program: load an AIPL-generated C program and keep
 *  it (and its spawned actors) alive, then exchange messages with the
 *  actors.  Backs the /actor/load and /actor/send HTTP endpoints.
 * ============================================================ */

static char          *g_res_arena;     /* kept alive: holds g_obj + literals */
static unsigned char *g_res_code;      /* kept alive: the JIT'd code          */
static long (*g_res_dispatch)(long, long, long, long, long, long);
static long (*g_res_methodid)(long);
static long (*g_res_nobj)(void);
static long (*g_res_clsname)(long);     /* __cls_name(cls) -> v_str  */
static long (*g_res_objcls)(long);      /* __obj_cls(id)   -> v_int  */
static long (*g_res_objfield)(long, long); /* __obj_field(id,fidx) -> value_t */

static void res_free(void)
{
    ap_killall();                        /* reap the previous resident's actor processes */
    if (g_res_code)  kfree(g_res_code);
    if (g_res_arena) kfree(g_res_arena);
    g_res_code = 0; g_res_arena = 0;
    g_res_dispatch = 0; g_res_methodid = 0; g_res_nobj = 0;
    g_res_clsname = 0; g_res_objcls = 0; g_res_objfield = 0;
}

int cc_actor_load(const char *src, int srclen, char *out, int outcap)
{
    res_free();                          /* drop any previous resident program */

    if (!arena_init(CC_ARENA)) {
        const char *m = "cc: out of memory"; int p = 0;
        while (m[p] && p < outcap - 1) { out[p] = m[p]; p++; } if (outcap) out[p] = 0;
        return -1;
    }
    g_err = 0; g_errbuf[0] = 0;

    unsigned long n = (unsigned long)(srclen < 0 ? 0 : srclen);
    char *s = (char *)cc_alloc(n + 1);
    for (unsigned long i = 0; i < n; i++) s[i] = src[i];
    s[n] = 0;

    token_t *toks = cc_lex(s);
    func_t  *fns  = cc_failed() ? 0 : cc_parse(toks);

    unsigned char *code = 0;
    int entry = 0, len = -1;
    if (!cc_failed()) {
        code = (unsigned char *)kmalloc(CC_CODECAP);
        if (!code) { arena_free(); return -1; }
        len = cc_codegen(fns, code, CC_CODECAP, &entry);
    }
    if (cc_failed() || len < 0) {
        int p = 0; const char *pfx = "cc: ";
        while (*pfx && p < outcap - 1) out[p++] = *pfx++;
        const char *e = g_errbuf[0] ? g_errbuf : "compile error";
        while (*e && p < outcap - 1) out[p++] = *e++;
        if (outcap) out[p] = 0;
        if (code) kfree(code);
        arena_free();
        return -1;
    }

    int doff = cc_func_offset("dispatch");
    int moff = cc_func_offset("__method_id");
    int noff = cc_func_offset("__nobj");
    int aoff = cc_func_offset("apply");
    int cnoff = cc_func_offset("__cls_name");
    int ocoff = cc_func_offset("__obj_cls");
    int ofoff = cc_func_offset("__obj_field");

    cc_sync_icache(code, (unsigned long)len);

    /* Run main() once to spawn/init the actors; capture its output, then
     * drain any asynchronous `send` cascade it kicked off. */
    cc_set_deadline();
    vheap_reset();
    lheap_reset();
    llm_session_reset();        /* a freshly-loaded resident starts a new chat */
    ap_reset();
    g_active_dispatch = (doff >= 0) ? (void *)(code + doff) : 0;
    ap_set_dispatch(g_active_dispatch);
    cc_set_apply((aoff >= 0) ? (void *)(code + aoff) : 0);
    g_cap = out; g_capcap = outcap; g_caplen = 0; if (outcap > 0) out[0] = 0;
    long (*mainfn)(void) = (long (*)(void))(code + entry);
    proc_actor_pump_enter();    /* actors run cooperatively (no timer preempt) */
    app_phase("ld-main");
    aipl_lock();                /* hold the vheap across main()'s direct vheap use */
    mainfn();                   /* spawn actor processes + initial sends */
    aipl_unlock();              /* release before ap_run (actors lock individually) */
    app_phase("ld-run");
    ap_run();                   /* drive them; actors persist (blocked) for /actor/send */
    proc_actor_pump_leave();
    if (g_cap) g_cap[(g_caplen < g_capcap) ? g_caplen : (g_capcap - 1)] = 0;
    g_cap = 0;

    /* Keep the code + arena resident (do NOT free): g_obj and the string
     * literals live in the arena, so the actors persist for later sends. */
    g_res_arena    = g_arena; g_arena = 0;       /* detach from the allocator */
    g_res_code     = code;
    g_res_dispatch = (doff >= 0) ? (void *)(code + doff) : 0;
    g_res_methodid = (moff >= 0) ? (void *)(code + moff) : 0;
    g_res_nobj     = (noff >= 0) ? (void *)(code + noff) : 0;
    g_res_clsname  = (cnoff >= 0) ? (void *)(code + cnoff) : 0;
    g_res_objcls   = (ocoff >= 0) ? (void *)(code + ocoff) : 0;
    g_res_objfield = (ofoff >= 0) ? (void *)(code + ofoff) : 0;

    /* Append the actor count. */
    long nobj = g_res_nobj ? v_int_of(g_res_nobj()) : 0;
    int p = 0; while (out[p] && p < outcap - 1) p++;
    const char *tag = "[resident: ";
    while (*tag && p < outcap - 1) out[p++] = *tag++;
    char nb[24]; const char *ns = v_render(v_int(nobj), nb, sizeof nb);
    while (*ns && p < outcap - 1) out[p++] = *ns++;
    const char *tail = " actor(s) live]\n";
    while (*tail && p < outcap - 1) out[p++] = *tail++;
    if (outcap) out[p] = 0;
    return 0;
}

int cc_actor_send_msg(int actor, const char *method, long a0, long a1, long a2,
                      char *out, int outcap)
{
    if (!g_res_dispatch || !g_res_methodid) {
        const char *m = "no resident actor program (POST /actor/load first)\n";
        int p = 0; while (m[p] && p < outcap - 1) { out[p] = m[p]; p++; } if (outcap) out[p] = 0;
        return -1;
    }

    cc_set_deadline();                   /* runaway guard; keep vheap (persistent fields) */

    /* Copy the method name into an 8-aligned buffer: a value_t string must
     * have its low bit clear (else it is misread as an immediate int). */
    static char methbuf[64] __attribute__((aligned(16)));
    int mi = 0; while (method[mi] && mi < (int)sizeof(methbuf) - 1) { methbuf[mi] = method[mi]; mi++; }
    methbuf[mi] = 0;

    long mid = v_int_of(g_res_methodid(v_str(methbuf)));
    if (mid < 0) {
        const char *m = "unknown method\n";
        int p = 0; while (m[p] && p < outcap - 1) { out[p] = m[p]; p++; } if (outcap) out[p] = 0;
        return -1;
    }

    proc_actor_pump_enter();             /* actors run cooperatively (no timer preempt) */
    app_phase("disp");
    aipl_lock();                         /* serialize the direct handler's vheap use */
    long res = g_res_dispatch((long)actor, mid, v_int(a0), v_int(a1), v_int(a2), v_int(0));
    aipl_unlock();
    ap_note_msg(actor);

    /* Drain any asynchronous `send`s the handler triggered (delivered to
     * the resident actor processes). */
    g_active_dispatch = g_res_dispatch;
    ap_set_dispatch(g_res_dispatch);
    app_phase("snd-run");
    ap_run();
    proc_actor_pump_leave();

    char tmp[64];
    const char *r = v_render(res, tmp, sizeof tmp);
    int p = 0; while (r[p] && p < outcap - 1) { out[p] = r[p]; p++; } if (outcap) out[p] = 0;
    return 0;
}

/* Like cc_actor_send_msg but the argument is a STRING (a value_t string),
 * and the reply (often a long string, e.g. an LLM turn) is copied whole into
 * `out`.  Backs /chat: message a resident actor's method to converse. */
int cc_actor_send_str(int actor, const char *method, const char *strarg, char *out, int outcap)
{
    if (!g_res_dispatch || !g_res_methodid) {
        const char *m = "no resident actor program (POST /actor/load first)\n";
        int p = 0; while (m[p] && p < outcap - 1) { out[p] = m[p]; p++; } if (outcap) out[p] = 0;
        return -1;
    }
    cc_set_deadline();
    static char methbuf[64] __attribute__((aligned(16)));
    int mi = 0; while (method[mi] && mi < (int)sizeof(methbuf) - 1) { methbuf[mi] = method[mi]; mi++; }
    methbuf[mi] = 0;
    long mid = v_int_of(g_res_methodid(v_str(methbuf)));
    if (mid < 0) {
        const char *m = "unknown method\n";
        int p = 0; while (m[p] && p < outcap - 1) { out[p] = m[p]; p++; } if (outcap) out[p] = 0;
        return -1;
    }
    /* 8-aligned NUL-terminated copy so v_str() is a valid string value_t */
    static char argbuf[1024] __attribute__((aligned(16)));
    int ai = 0; while (strarg && strarg[ai] && ai < (int)sizeof(argbuf) - 1) { argbuf[ai] = strarg[ai]; ai++; }
    argbuf[ai] = 0;

    proc_actor_pump_enter();             /* actors run cooperatively (no timer preempt) */
    app_phase("disp-s");
    aipl_lock();                         /* serialize the direct handler's vheap use */
    long res = g_res_dispatch((long)actor, mid, v_str(argbuf), v_int(0), v_int(0), v_int(0));
    aipl_unlock();
    g_active_dispatch = g_res_dispatch; ap_set_dispatch(g_res_dispatch); ap_run();
    proc_actor_pump_leave();

    char tmp[64];
    const char *r = v_render(res, tmp, sizeof tmp);   /* string -> returns the pointer itself */
    int p = 0; while (r[p] && p < outcap - 1) { out[p] = r[p]; p++; } if (outcap) out[p] = 0;
    ap_note_msg(actor);
    return 0;
}

/* Class name of resident actor `apid` (g_obj is indexed by actor id) into
 * `out`; returns its length, or 0 if no resident program / no name table.
 * Used by the wm "Actors (live)" window. */
int cc_actor_name(int apid, char *out, int cap)
{
    if (cap <= 0) return 0;
    out[0] = 0;
    if (!g_res_objcls || !g_res_clsname) return 0;
    /* __obj_cls(int id) takes a RAW actor id and returns v_int(cls);
     * __cls_name(int cls) takes a RAW class index — do NOT v_int()-tag the
     * args (g_obj[] is indexed raw; cls is compared to raw 0,1,...). */
    long cls = v_int_of(g_res_objcls((long)apid));
    long nv  = g_res_clsname(cls);
    char tmp[40];
    const char *r = v_render(nv, tmp, sizeof tmp);
    int n = 0; while (r[n] && n < cap - 1) { out[n] = r[n]; n++; }
    out[n] = 0;
    return n;
}

/* Integer value of field `fidx` of resident actor `apid` (declaration order:
 * for the Philosopher class field 1 is the meal count).  Returns -1 if there
 * is no resident program / no field accessor.  Used by the Actors window. */
long cc_actor_field(int apid, int fidx)
{
    if (!g_res_objfield) return -1;
    return v_int_of(g_res_objfield((long)apid, (long)fidx));
}

/* ---- shell front-ends (also let the HTTP-only feature be tested on QEMU) ---- */

static int cc_atoi(const char *s)
{
    int n = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return neg ? -n : n;
}

int cmd_aload(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: aload <file.c>  (load a resident actor program)\n"); return -1; }
    vfs_node_t *f = vfs_resolve(argv[1]);
    if (!f || f->kind != VFS_FILE) { uart_puts("aload: no such file\n"); return -1; }
    static char out[4096];
    int rc = cc_actor_load((const char *)f->data, (int)f->size, out, sizeof out);
    uart_puts(out);
    return rc;
}

int cmd_amsg(int argc, char **argv)
{
    if (argc < 3) { uart_puts("usage: amsg <actor> <method> [arg]\n"); return -1; }
    int  actor = cc_atoi(argv[1]);
    long arg   = (argc >= 4) ? cc_atoi(argv[3]) : 0;
    static char out[256];
    cc_actor_send_msg(actor, argv[2], arg, 0, 0, out, sizeof out);
    uart_puts(out); uart_puts("\n");
    return 0;
}
