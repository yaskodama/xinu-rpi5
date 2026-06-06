// cc/cc.c — on-device C compiler driver + runtime for the Pi 5.
//
// Pipeline: C source -> cc_lex -> cc_parse -> cc_codegen (AArch64 machine
// code into a kmalloc'd, executable heap buffer) -> flush caches -> call
// the compiled main() in place (JIT) -> capture its output + return value.
//
// This is the lean Pi 5 port of the xinu-rpi4 compiler: the lexer, parser
// and AArch64 code generator (parse.c / codegen.c / ccpriv.h) are shared
// verbatim; only this driver is trimmed — no AIPL value_t runtime, actor
// pump, or gfx builtins.  Compiled code reaches the kernel through
// cc_resolve_extern (print/putchar/puts/actor_send), the JIT's call seam.
//
// JIT correctness on Pi 5: the MMU is on with an identity map but caches
// OFF (SCTLR.C=0, I=0) and the low-RAM heap is mapped RWX (no XN bit), so
// codegen's stores and the CPU's instruction fetches both go straight to
// RAM — freshly emitted code is immediately executable.  cc_sync_icache()
// still issues the canonical dc/ic maintenance + barriers so the path is
// correct even if a future build turns the caches on.

#include "ccpriv.h"
#include "uart.h"
#include "kmalloc.h"
#include "actor.h"

extern void kfree(void *p);

#define CC_ARENA    (2 * 1024 * 1024)  /* lexer/parser arena (matches Pi 4)   */
#define CC_CODECAP  (256 * 1024)       /* JIT code buffer                     */

/* ---------- arena allocator (whole-compile lifetime) ---------- */

/* Static buffers, NOT kmalloc: the JIT runs inside the timer-ISR RX dispatch,
 * and the heap allocator is not reentrant against the main thread.  Only one
 * cc_run_source() runs at a time (the ISR can't preempt itself and the wm-loop
 * drain is guarded), so a single static arena + code buffer is safe.  The code
 * buffer lives in BSS, which the identity MMU maps Normal + executable (no XN),
 * so JIT'd bytes run in place. */
static unsigned char  g_codebuf[CC_CODECAP] __attribute__((aligned(64)));
static char           g_arenabuf[CC_ARENA];
static char          *g_arena;
static unsigned long  g_acap, g_apos;
static char           g_panic[64];     /* safe sink once we OOM */

static int arena_init(unsigned long cap)
{
    g_arena = g_arenabuf;
    g_acap  = (cap <= sizeof g_arenabuf) ? cap : sizeof g_arenabuf;
    g_apos  = 0;
    return 1;
}
static void arena_free(void)
{
    g_arena = 0; g_acap = g_apos = 0;
}

void *cc_alloc(unsigned long n)
{
    n = (n + 15) & ~15UL;                  /* 16-byte align */
    if (!g_arena || g_apos + n > g_acap) { cc_error("compiler arena exhausted"); return g_panic; }
    void *p = g_arena + g_apos;
    g_apos += n;
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
/* codegen injects a call to cc_tick() at every loop back-edge; it returns
 * 1 while time remains and 0 once a wall-clock deadline passes, making the
 * generated code break out.  Time-based (generic timer) because caches are
 * off so per-iteration speed is unpredictable. */
/* The /cc HTTP handler runs the JIT inside the timer-ISR-driven RX dispatch
 * (genet_rx_tick), where DAIF.I is masked, so a long run freezes RX + the
 * display.  Keep the runaway deadline short — real demo programs finish in
 * microseconds; a runaway aborts in a quarter second. */
#define CC_TIMEOUT_MS 250
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

/* actor_send("counter", "bump", 0) — reach the live actor gateway from C. */
static long cc_actor_send(long id, const char *method, long arg)
{
    int out = 0;
    if (actor_message((int)id, method, (int)arg, &out) != 0) return -1;
    return (long)out;
}

/* ===================== AIPL value_t runtime (ported from xinu-rpi4) =====================
 * A tagged 64-bit value system (int/string/float/list) so the host AIPL->C compiler's
 * --xinu-jit output runs on this JIT.  Self-contained: only emit_ch/emit_str + two static
 * bump heaps.  map/filter call back into a JIT'd apply(id,x) via cc_set_apply. */

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


/* ===================== cooperative actor pump + global GC =====================
 * The AIPL --xinu-jit backend emits an actor program: g_spawn() -> cc_actor_new
 * (raw id), `send a.m(x)` -> enqueue(to_raw, mid_raw, a0..a3 tagged), and a
 * dispatch(self, meth, a0..a3) router.  Instead of one Xinu process per actor
 * (Pi 4's actorproc), this is a single-threaded cooperative pump: a global FIFO
 * of messages drained through dispatch().  Each actor carries liveness + a
 * last-active timestamp so a GLOBAL garbage collector can reap idle/dead ones. */

#define ACT_MAX 1024
#define MQ_MAX  8192

static void cc_sync_icache(void *p, unsigned long len);   /* defined below */

static long (*g_dispatch)(long, long, long, long, long, long);  /* JIT'd dispatch */
static long (*g_res_methodid)(long);                            /* JIT'd __method_id */
static int  g_nact;                            /* high-water actor id */
static unsigned char  g_alive[ACT_MAX];
static unsigned char  g_protect[ACT_MAX];      /* GC-exempt */
static unsigned long  g_born[ACT_MAX];         /* cc_now() at spawn */
static unsigned long  g_lastact[ACT_MAX];      /* cc_now() of last dispatch */
static unsigned long  g_gc_runs, g_gc_killed;  /* lifetime GC tallies */

struct cc_msg { long to, mid, a0, a1, a2, a3; };
static struct cc_msg g_mq[MQ_MAX];
static int g_qh, g_qt;

static unsigned long cc_cntfrq(void)
{
    unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return f;
}
static unsigned long cc_idle_ms(int i)
{
    unsigned long frq = cc_cntfrq(); if (frq < 1000) frq = 1000;
    return (cc_now() - g_lastact[i]) / (frq / 1000UL);
}

static void cc_actor_reset(void)
{
    g_nact = 0; g_qh = g_qt = 0;
    for (int i = 0; i < ACT_MAX; i++) { g_alive[i]=0; g_protect[i]=0; g_born[i]=0; g_lastact[i]=0; }
}
static long cc_actor_new(void)
{
    if (g_nact >= ACT_MAX) return -1;
    int id = g_nact++;
    g_alive[id] = 1; g_protect[id] = 0; g_born[id] = cc_now(); g_lastact[id] = cc_now();
    return id;                                  /* RAW id (g_obj index) */
}
static long cc_actor_suicide(long self_v)
{
    int id = (int)v_int_of(self_v);
    if (id >= 0 && id < ACT_MAX) g_alive[id] = 0;
    return v_int(0);
}
static void cc_enqueue(long to, long mid, long a0, long a1, long a2, long a3)
{
    int t = (int)to;
    if (t < 0 || t >= ACT_MAX || !g_alive[t]) return;
    int nx = (g_qt + 1) % MQ_MAX;
    if (nx == g_qh) return;                      /* mailbox full: drop */
    g_mq[g_qt].to=to; g_mq[g_qt].mid=mid;
    g_mq[g_qt].a0=a0; g_mq[g_qt].a1=a1; g_mq[g_qt].a2=a2; g_mq[g_qt].a3=a3;
    g_qt = nx;
}
static void cc_pump(void)
{
    while (g_qh != g_qt) {
        if (!cc_tick()) break;                   /* runaway deadline */
        struct cc_msg m = g_mq[g_qh]; g_qh = (g_qh + 1) % MQ_MAX;
        int t = (int)m.to;
        if (g_dispatch && t >= 0 && t < ACT_MAX && g_alive[t]) {
            g_lastact[t] = cc_now();
            g_dispatch(m.to, m.mid, m.a0, m.a1, m.a2, m.a3);
        }
    }
}
/* `now a.m(x)` — synchronous call, returns the handler's value. */
static long cc_call(long self, long to, long method, long a0, long a1, long a2, long a3)
{
    (void)self;
    int t = (int)to;
    if (g_dispatch && t >= 0 && t < ACT_MAX && g_alive[t]) {
        g_lastact[t] = cc_now();
        return g_dispatch(to, method, a0, a1, a2, a3);
    }
    return v_int(0);
}

/* ---- GC primitives (also exposed to AIPL so a GC actor can be written in
 * AIPL itself) ---- */
static int gc_sweep_core(long threshold_ms, int dry, int *scanned)
{
    int killed = 0; if (scanned) *scanned = 0;
    for (int i = 0; i < g_nact; i++) {
        if (!g_alive[i]) continue;
        if (scanned) (*scanned)++;
        if (g_protect[i]) continue;
        if ((long)cc_idle_ms(i) >= threshold_ms) {
            if (!dry) g_alive[i] = 0;
            killed++;
        }
    }
    if (!dry) { g_gc_runs++; g_gc_killed += (unsigned long)killed; }
    return killed;
}
static long cc_actor_count(void)               { int n=0; for(int i=0;i<g_nact;i++) if(g_alive[i]) n++; return v_int(n); }
static long cc_actor_alive(long id)            { int i=(int)v_int_of(id); return v_int(i>=0&&i<ACT_MAX&&g_alive[i]); }
static long cc_actor_age(long id)              { int i=(int)v_int_of(id); return v_int(i>=0&&i<ACT_MAX?(long)cc_idle_ms(i):0); }
static long cc_actor_kill(long id)             { int i=(int)v_int_of(id); if(i>=0&&i<ACT_MAX&&g_alive[i]){g_alive[i]=0;return v_int(1);} return v_int(0); }
static long cc_actor_protect(long id, long on) { int i=(int)v_int_of(id); if(i>=0&&i<ACT_MAX) g_protect[i]=(unsigned char)v_int_of(on); return v_int(0); }
static long cc_actor_protected(long id)        { int i=(int)v_int_of(id); return v_int(i>=0&&i<ACT_MAX?g_protect[i]:0); }
static long cc_gc_sweep(long threshold_ms, long dry)
{
    int scanned = 0;
    return v_int(gc_sweep_core((long)v_int_of(threshold_ms), (int)v_int_of(dry), &scanned));
}
static long cc_now_ms(void) { unsigned long frq=cc_cntfrq(); if(frq<1000)frq=1000; return v_int((long)(cc_now()/(frq/1000UL))); }

/* select/saga/crash: not supported by the cooperative pump — resolve to inert
 * stubs so programs that merely reference them still JIT (most don't use them). */
static long g_sel[4];
static long cc_select(long s,long n,long m0,long m1,long m2,long m3){(void)s;(void)n;(void)m0;(void)m1;(void)m2;(void)m3;return v_int(-1);}
static long cc_sel_arg(long i){return (i>=0&&i<4)?g_sel[i]:v_int(0);}
static long cc_saga_reset(void){return v_int(0);}
static long cc_saga_fail(void){return v_int(0);}
static long cc_saga_failed(void){return 0;}
static long cc_crash(void){return v_int(0);}
static long cc_crashed_value(void){return v_int(-999);}

/* ---------- public: load a resident actor program / message it / GC it ----------
 * Shares g_codebuf + the arena with /cc, so /cc and /actor/load are mutually
 * exclusive (running /cc invalidates a loaded program). */
static int  g_res_loaded;

int cc_actor_load(const char *src, int srclen, char *out, int outcap)
{
    unsigned long n = (unsigned long)(srclen < 0 ? 0 : srclen);
    g_res_loaded = 0; g_dispatch = 0; g_res_methodid = 0;
    if (!arena_init(CC_ARENA)) return -2;
    g_err = 0; g_errbuf[0] = 0;

    char *s = (char *)cc_alloc(n + 1);
    for (unsigned long i = 0; i < n; i++) s[i] = src[i];
    s[n] = 0;

    token_t *toks = cc_lex(s);
    func_t  *fns  = cc_failed() ? 0 : cc_parse(toks);
    int entry = 0, len = -1;
    if (!cc_failed()) len = cc_codegen(fns, g_codebuf, CC_CODECAP, &entry);
    if (cc_failed() || len < 0) {
        int p = 0; const char *e = g_errbuf[0] ? g_errbuf : "compile error";
        const char *pfx = "cc: "; while (*pfx && p < outcap-1) out[p++] = *pfx++;
        while (*e && p < outcap-1) out[p++] = *e++; out[p] = 0;
        /* keep the arena (no free): nothing resident, but harmless */
        return -1;
    }
    cc_sync_icache(g_codebuf, (unsigned long)len);

    /* Fresh actor world + value heaps; wire dispatch/__method_id/apply. */
    cc_actor_reset(); vheap_reset(); lheap_reset();
    int doff = cc_func_offset("dispatch");
    int moff = cc_func_offset("__method_id");
    int aoff = cc_func_offset("apply");
    g_dispatch     = (doff >= 0) ? (void *)(g_codebuf + doff) : 0;
    g_res_methodid = (moff >= 0) ? (void *)(g_codebuf + moff) : 0;
    cc_set_apply(aoff >= 0 ? (void *)(g_codebuf + aoff) : 0);
    cc_set_deadline();

    g_cap = out; g_capcap = outcap; g_caplen = 0;
    long (*mainfn)(void) = (long (*)(void))(g_codebuf + entry);
    mainfn();                 /* spawn actors + initial sends */
    cc_pump();                /* deliver the initial cascade (deadline-bounded) */
    /* NOTE: arena deliberately NOT freed — the JIT'd code holds pointers to
     * string literals living in it; they must survive for later /actor/send. */

    /* Summary into the capture buffer. */
    int alive = 0; for (int i = 0; i < g_nact; i++) if (g_alive[i]) alive++;
    const char *m1 = "\nactors: "; for (int i=0;m1[i]&&g_caplen<g_capcap-1;i++) g_cap[g_caplen++]=m1[i];
    { char t[16]; int tn=0,v=alive; if(!v)t[tn++]='0'; while(v){t[tn++]=(char)('0'+v%10);v/=10;} while(tn&&g_caplen<g_capcap-1)g_cap[g_caplen++]=t[--tn]; }
    const char *m2 = " live\n"; for (int i=0;m2[i]&&g_caplen<g_capcap-1;i++) g_cap[g_caplen++]=m2[i];
    g_cap[(g_caplen<g_capcap)?g_caplen:(g_capcap-1)] = 0; g_cap = 0;
    g_res_loaded = 1;
    return 0;
}

int cc_actor_send_msg(int to, const char *method, int arg, char *out, int outcap)
{
    if (!g_res_loaded || !g_dispatch || !g_res_methodid) {
        const char *e = "no resident actor program (POST /actor/load first)\n";
        int p=0; while (*e && p<outcap-1) out[p++]=*e++; out[p]=0; return -1;
    }
    long mid = g_res_methodid(v_str(method));
    int midraw = (int)v_int_of(mid);
    if (midraw < 0) {
        const char *e = "unknown method\n"; int p=0; while(*e&&p<outcap-1)out[p++]=*e++; out[p]=0; return -1;
    }
    cc_set_deadline();
    g_cap = out; g_capcap = outcap; g_caplen = 0;
    long res = g_dispatch((long)to, (long)midraw, v_int(arg), v_int(0), v_int(0), v_int(0));
    cc_pump();
    /* append "=> <rendered result>" */
    const char *arrow = "=> "; for (int i=0;arrow[i]&&g_caplen<g_capcap-1;i++) g_cap[g_caplen++]=arrow[i];
    { char rb[32]; const char *rs = v_render(res, rb, sizeof rb); for(int i=0;rs[i]&&g_caplen<g_capcap-1;i++) g_cap[g_caplen++]=rs[i]; }
    if (g_caplen<g_capcap-1) g_cap[g_caplen++]='\n';
    g_cap[(g_caplen<g_capcap)?g_caplen:(g_capcap-1)] = 0; g_cap = 0;
    return 0;
}

/* Global GC entry for the HTTP endpoint + the periodic timer sweep.  Writes a
 * one-line report into `out` (may be NULL for the silent periodic call). */
int cc_actor_gc(long threshold_ms, int dry, char *out, int outcap)
{
    if (!g_res_loaded) { if (out&&outcap>0){ const char*e="no resident actors\n"; int p=0; while(*e&&p<outcap-1)out[p++]=*e++; out[p]=0;} return 0; }
    int scanned = 0;
    int killed = gc_sweep_core(threshold_ms, dry, &scanned);
    if (out && outcap > 0) {
        int p = 0;
        const char *a = dry ? "gc(dry): scanned " : "gc: scanned ";
        while (*a && p<outcap-1) out[p++]=*a++;
        char t[16]; int tn,v;
        v=scanned; tn=0; if(!v)t[tn++]='0'; while(v){t[tn++]=(char)('0'+v%10);v/=10;} while(tn&&p<outcap-1)out[p++]=t[--tn];
        const char *b = dry ? " would-reap " : " reaped ";
        while (*b && p<outcap-1) out[p++]=*b++;
        v=killed; tn=0; if(!v)t[tn++]='0'; while(v){t[tn++]=(char)('0'+v%10);v/=10;} while(tn&&p<outcap-1)out[p++]=t[--tn];
        if (p<outcap-1) out[p++]='\n'; out[p]=0;
    }
    return killed;
}

/* On-screen stats accessors. */
unsigned long cc_gc_run_count(void)   { return g_gc_runs; }
unsigned long cc_gc_kill_count(void)  { return g_gc_killed; }
int           cc_actor_live_count(void){ int n=0; for(int i=0;i<g_nact;i++) if(g_alive[i]) n++; return n; }
int           cc_actor_resident(void) { return g_res_loaded; }

/* ---------- extern-symbol table: the JIT's call-into-kernel seam ---------- */

unsigned long cc_resolve_extern(const char *name)
{
    struct { const char *n; void *f; } tab[] = {
        { "print",      (void *)&cc_print      },
        { "putchar",    (void *)&cc_putchar    },
        { "putc",       (void *)&cc_putchar    },
        { "puts",       (void *)&cc_puts       },
        { "actor_send", (void *)&cc_actor_send },
        { "__cc_tick",  (void *)&cc_tick       },
        /* AIPL value_t runtime — the --xinu-jit backend's call seam. */
        { "v_int",       (void *)&v_int        },
        { "v_str",       (void *)&v_str        },
        { "v_floatlit",  (void *)&v_floatlit   },
        { "v_add",       (void *)&v_add        },
        { "v_sub",       (void *)&v_sub        },
        { "v_mul",       (void *)&v_mul        },
        { "v_div",       (void *)&v_div        },
        { "v_lt",        (void *)&v_lt         },
        { "v_le",        (void *)&v_le         },
        { "v_eq",        (void *)&v_eq         },
        { "v_ne",        (void *)&v_ne         },
        { "v_and",       (void *)&v_and        },
        { "v_or",        (void *)&v_or         },
        { "v_not",       (void *)&v_not        },
        { "v_print",     (void *)&v_print      },
        { "v_truthy",    (void *)&v_truthy_x   },
        { "v_int_of",    (void *)&v_int_of     },
        { "v_list_new",   (void *)&v_list_new   },
        { "v_list_push",  (void *)&v_list_push  },
        { "v_list_set",   (void *)&v_list_set   },
        { "v_list_get",   (void *)&v_list_get   },
        { "v_list_len",   (void *)&v_list_len   },
        { "v_list_zeros", (void *)&v_list_zeros },
        { "v_list_map",   (void *)&v_list_map   },
        { "v_list_filter",(void *)&v_list_filter},
        /* AIPL actor model (cooperative pump) + GC primitives. */
        { "enqueue",          (void *)&cc_enqueue        },
        { "cc_actor_new",     (void *)&cc_actor_new      },
        { "cc_actor_suicide", (void *)&cc_actor_suicide  },
        { "cc_call",          (void *)&cc_call           },
        { "cc_select",        (void *)&cc_select         },
        { "cc_sel_arg",       (void *)&cc_sel_arg        },
        { "cc_actor_count",   (void *)&cc_actor_count    },
        { "cc_actor_alive",   (void *)&cc_actor_alive    },
        { "cc_actor_age",     (void *)&cc_actor_age      },
        { "cc_actor_kill",    (void *)&cc_actor_kill     },
        { "cc_actor_protect", (void *)&cc_actor_protect  },
        { "cc_actor_protected",(void *)&cc_actor_protected},
        { "cc_gc_sweep",      (void *)&cc_gc_sweep       },
        { "cc_now_ms",        (void *)&cc_now_ms         },
        { "cc_saga_reset",    (void *)&cc_saga_reset     },
        { "cc_saga_fail",     (void *)&cc_saga_fail      },
        { "cc_saga_failed",   (void *)&cc_saga_failed    },
        { "cc_crash",         (void *)&cc_crash          },
        { "cc_crashed_value", (void *)&cc_crashed_value  },
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
    for (unsigned long a = start; a < end; a += 64) {
        __asm__ volatile ("dc cvau,  %0" :: "r"(a) : "memory");
        __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile ("dsb sy" ::: "memory");
    for (unsigned long a = start; a < end; a += 64)
        __asm__ volatile ("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile ("ic iallu" ::: "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");
}

/* ---------- compile + JIT-execute one translation unit ---------- */

static int compile_run_core(const char *src, unsigned long n, long *retval)
{
    /* /cc reuses g_codebuf + the arena, so any resident actor program is now
     * invalid (its dispatch + string literals are about to be overwritten). */
    g_res_loaded = 0; g_dispatch = 0; g_res_methodid = 0;
    if (!arena_init(CC_ARENA)) return -2;
    g_err = 0; g_errbuf[0] = 0;

    char *s = (char *)cc_alloc(n + 1);
    for (unsigned long i = 0; i < n; i++) s[i] = src[i];
    s[n] = 0;

    token_t *toks = cc_lex(s);
    func_t  *fns  = cc_failed() ? 0 : cc_parse(toks);

    unsigned char *code = g_codebuf;
    int entry = 0, len = -1;
    if (!cc_failed())
        len = cc_codegen(fns, code, CC_CODECAP, &entry);
    if (cc_failed() || len < 0) { arena_free(); return -1; }

    cc_sync_icache(code, (unsigned long)len);
    cc_set_deadline();

    /* Fresh value_t heaps per run, and register the JIT'd apply(id,x) so
     * v_list_map/filter can call back into compiled AIPL functions. */
    vheap_reset();
    lheap_reset();
    int aoff = cc_func_offset("apply");
    cc_set_apply(aoff >= 0 ? (void *)(code + aoff) : 0);

    long (*entryfn)(void) = (long (*)(void))(code + entry);
    long rc = entryfn();                 /* JIT: run main() in place */
    if (retval) *retval = rc;

    arena_free();
    return 0;
}

/* ---------- public API: compile + run, capture output into `out` ----------
 * Returns 0 on success (*retval = main()'s return), -1 compile error, -2 OOM.
 * On error `out` holds "cc: <message>". */
int cc_run_source(const char *src, int srclen, char *out, int outcap, long *retval)
{
    g_cap = out; g_capcap = outcap; g_caplen = 0;
    if (out && outcap > 0) out[0] = 0;

    long rv = 0;
    int rc = compile_run_core(src, (unsigned long)(srclen < 0 ? 0 : srclen), &rv);

    if (rc == 0 && g_aborted && g_cap) {
        const char *note = "cc: aborted (ran past the runaway-loop deadline)\n";
        for (int i = 0; note[i] && g_caplen < g_capcap - 1; i++) g_cap[g_caplen++] = note[i];
    }
    if (g_cap) { g_cap[(g_caplen < g_capcap) ? g_caplen : (g_capcap - 1)] = 0; }
    g_cap = 0;

    if (rc == 0) { if (retval) *retval = rv; return 0; }

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
