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

#define CC_ARENA    (256 * 1024)   /* lexer/parser arena (whole-compile life) */
#define CC_CODECAP  (128 * 1024)   /* JIT code buffer                         */

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
