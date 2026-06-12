// kernel/shell.c — bare-metal Xinu-style xsh on the boot UART.
//
// Follows the davidxyz/xinuPi shell.c shape: a `centry` table maps
// command name → handler.  We don't have thread, tty, file
// descriptors or printf yet on Pi 5 AArch64, so each handler talks
// directly to the UART through the helpers in uart.h.
//
// The REPL loop:
//   1. print SHELL_PROMPT
//   2. uart_getline() — blocks for a CR/LF-terminated line
//   3. tokenise on ASCII whitespace
//   4. linear search the command table; on hit, call the handler
//   5. on miss, print "?command-not-found" and loop
//
// Built-in commands:
//   help                 list the registered commands
//   echo  <words...>     print the rest of the line verbatim
//   hello                friendly greeting (smoke marker)
//   mem                  link-script symbols: __bss_start, __bss_end, _end
//   peek  <hex_addr>     read 32-bit word at MMIO address (default base
//                        BCM2712 — try `peek 0x107d001018` for UART_FR)
//   uptime               printed since this is bare metal: stub "?", later
//                        bound to the generic timer in phase S1
//   ps                   tiny core-status table (the scheduler isn't
//                        up until phase S0, so we just list MPIDR /
//                        CurrentEL and mark cores 1-3 as WFE-parked
//                        per boot.S)
//   halt                 mask interrupts, ask PSCI SYSTEM_OFF
//                        (HVC at EL2, SMC at EL1 — QEMU virt
//                        actually exits; real Pi 5 falls through to
//                        the WFE park)
//   pingpong [N]         run a 2-actor AIPL-style PingPong: two
//                        cooperative "processes" exchange messages
//                        through 1-slot inboxes for N rounds (default
//                        5, capped at 50) then self-terminate when
//                        both stop sending and the inboxes drain
//   reboot               full SoC reset via the BCM2712 PM watchdog
//                        (0x10_7D200000, bcm2835 RSTC/WDOG layout)
//
// Designed so phase S0 (thread switch) and S1 (clock IRQ) can later
// replace these stubs without touching the dispatch code.

#include "uart.h"
#include "shell.h"
#include "memory.h"
#include "proc.h"
#include "usb.h"
#include "timer.h"
#include "wm.h"
#include "genet.h"
#include "vfs.h"
#include "wifi.h"

/* Linker-script symbols (from kernel/link.ld).  `_end` is already
 * declared (as `unsigned char _end[]`) by memory.h, which we include
 * above — redeclaring it here as `unsigned long` is a type conflict
 * that breaks a clean build, so we rely on memory.h's declaration. */
extern unsigned long __bss_start;
extern unsigned long __bss_end;

/* ---------- helpers (no libc available) ---------- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == 0 && *b == 0);
}

static int str_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Render a non-negative integer in base 10. */
static void puts_dec(int v)
{
    char buf[12];
    int n = 0;
    if (v < 0) { uart_putc('-'); v = -v; }
    if (v == 0) { uart_putc('0'); return; }
    while (v > 0) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) uart_putc(buf[n]);
}

/* Render an unsigned long in hex (0x… prefix, lowercase). */
static void puts_hex(unsigned long v)
{
    char buf[2 + 16 + 1];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        unsigned long nyb = (v >> ((15 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10));
    }
    buf[18] = 0;
    uart_puts(buf);
}

/* Parse a hex string like "0x107d001018" or "107d001018" → unsigned long.
 * Returns 0 on parse error (caller checks the original token if it cares). */
static unsigned long parse_hex(const char *s)
{
    unsigned long v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++;
        unsigned long d;
        if (c >= '0' && c <= '9') d = (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned long)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    return v;
}

/* ---------- command implementations ---------- */

typedef int (*cmd_fn)(int argc, char **argv);

struct centry {
    const char *name;
    const char *help;
    cmd_fn      fn;
};

static const struct centry commandtab[];   /* forward */

static int cmd_help(int argc, char **argv)
{
    int i;
    (void)argc; (void)argv;
    uart_puts("available commands:\n");
    for (i = 0; commandtab[i].name; i++) {
        uart_puts("  ");
        uart_puts(commandtab[i].name);
        /* pad to ~10 columns */
        int pad = 10 - str_len(commandtab[i].name);
        while (pad-- > 0) uart_putc(' ');
        uart_puts(commandtab[i].help);
        uart_puts("\n");
    }
    return 0;
}

/* ---------- hierarchical filesystem: pwd / cd / ls / cat ---------- */

static char g_cwd[128] = "/";          /* shell current directory (absolute) */

/* Resolve `arg` (absolute or relative to g_cwd) into a normalized absolute
 * path in `out`, collapsing "." and "..". */
static void fs_resolve(const char *arg, char *out, int outsz)
{
    char tmp[256]; int n = 0;
    if (arg && arg[0] == '/') {
        for (const char *p = arg; *p && n < (int)sizeof tmp - 1; p++) tmp[n++] = *p;
    } else {
        for (const char *p = g_cwd; *p && n < (int)sizeof tmp - 1; p++) tmp[n++] = *p;
        if (n == 0 || tmp[n-1] != '/') if (n < (int)sizeof tmp - 1) tmp[n++] = '/';
        for (const char *p = arg ? arg : ""; *p && n < (int)sizeof tmp - 1; p++) tmp[n++] = *p;
    }
    tmp[n] = 0;

    char comp[32][32]; int top = 0;
    char *s = tmp;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        char *start = s;
        while (*s && *s != '/') s++;
        int len = (int)(s - start);
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') { if (top > 0) top--; continue; }
        if (len > 31) len = 31;
        if (top < 32) { for (int i = 0; i < len; i++) comp[top][i] = start[i]; comp[top][len] = 0; top++; }
    }
    int o = 0;
    if (top == 0) { if (outsz > 1) { out[0] = '/'; out[1] = 0; } return; }
    for (int i = 0; i < top; i++) {
        if (o < outsz - 1) out[o++] = '/';
        for (char *c = comp[i]; *c && o < outsz - 1; c++) out[o++] = *c;
    }
    out[o] = 0;
}

static int cmd_pwd(int argc, char **argv)
{
    (void)argc; (void)argv;
    uart_puts(g_cwd); uart_putc('\n');
    return 0;
}

static int cmd_cd(int argc, char **argv)
{
    char path[128];
    fs_resolve(argc >= 2 ? argv[1] : "/", path, sizeof path);
    vfs_node_t *n = vfs_lookup(path);
    if (!n)               { uart_puts("cd: no such path: "); uart_puts(argc>=2?argv[1]:"/"); uart_putc('\n'); return 0; }
    if (n->kind != VFS_DIR){ uart_puts("cd: not a directory\n"); return 0; }
    int i = 0; for (; path[i] && i < (int)sizeof g_cwd - 1; i++) g_cwd[i] = path[i]; g_cwd[i] = 0;
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    char path[128];
    fs_resolve(argc >= 2 ? argv[1] : "", path, sizeof path);
    vfs_node_t *n = vfs_lookup(path);
    if (!n) { uart_puts("ls: no such path: "); uart_puts(argc>=2?argv[1]:g_cwd); uart_putc('\n'); return 0; }
    if (n->kind == VFS_FILE) { uart_puts(n->name); uart_putc('\n'); return 0; }
    for (vfs_node_t *c = n->children; c; c = c->next) {
        uart_puts(c->name);
        if (c->kind == VFS_DIR) uart_putc('/');
        uart_putc('\n');
    }
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: cat <file>\n"); return 0; }
    char path[128];
    fs_resolve(argv[1], path, sizeof path);
    vfs_node_t *n = vfs_lookup(path);
    if (!n)                { uart_puts("cat: no such file: "); uart_puts(argv[1]); uart_putc('\n'); return 0; }
    if (n->kind != VFS_FILE){ uart_puts("cat: is a directory\n"); return 0; }
    char buf[1024];
    int r = vfs_read(n, buf, sizeof buf - 1);
    for (int i = 0; i < r; i++) uart_putc(buf[i]);
    uart_putc('\n');
    return 0;
}

/* kexec <file> — boot a different Xinu kernel image from /microsd (no reflash).
 * e.g.  kexec /microsd/KERNEL_2712.IMG   — RAM-only chainload, does not return. */
static int cmd_kexec(int argc, char **argv)
{
    extern int microsd_kexec(const char *path);
    if (argc < 2) { uart_puts("usage: kexec /microsd/<kernel.img>\n"); return 0; }
    char path[128];
    fs_resolve(argv[1], path, sizeof path);
    uart_puts("kexec: loading "); uart_puts(path); uart_puts(" ...\n");
    int r = microsd_kexec(path);     /* returns only on failure */
    uart_puts("kexec: failed (rc="); uart_putc((char)('0' + (r<0?1:0))); uart_puts(") — not booted\n");
    return 0;
}

/* ---- local C/AIPL runner: compile, then execute in a dedicated process ----
 * `cc` and `make` compile a program and run it on its OWN process stack, so a
 * buggy program (deep recursion, runaway loop, bad pointer) can only blow that
 * stack and trip the runaway-loop deadline — it can never smash the shell /
 * network-ISR stack and freeze the board, which is exactly what the old inline
 * JIT path did (e.g. the AIPL value_t sample).  The inline JIT (cc_run_source)
 * stays reserved for the external HTTP path, where an outside computer streams
 * an AIPL program in and gets its output back. */
#define CCRUN_STK   16384

static const char *g_ccrun_src;     /* source handed to the runner process    */
static int         g_ccrun_len;
static char        g_ccrun_out[2048];
static long        g_ccrun_rv;
static int         g_ccrun_rc;      /* cc_compile() result: 0 ok, <0 error     */
static int         g_ccrun_aborted;

/* Runs in the dedicated cc-run process: BOTH the compile and the execute
 * happen here, so the whole JIT machinery uses this process's own stack and
 * can't corrupt the shell / network-ISR stack. */
static void ccrun_proc_entry(void)
{
    extern int  cc_compile(const char *, int);
    extern long cc_exec_compiled(char *, int, int *);
    g_ccrun_rc = cc_compile(g_ccrun_src, g_ccrun_len);
    if (g_ccrun_rc == 0)
        g_ccrun_rv = cc_exec_compiled(g_ccrun_out, sizeof g_ccrun_out, &g_ccrun_aborted);
    proc_exit();
}

/* A .abcl/.aipl source is real AIPL — translate it to C with the on-device
 * abcl2c first, then hand the C to cc.  g_xlat holds the translation. */
static char g_xlat[16384];
static int  src_is_aipl(const char *path)
{
    int n = 0; while (path[n]) n++;
    const char *e[] = { ".abcl", ".aipl", 0 };
    for (int k = 0; e[k]; k++) { int el = 0; while (e[k][el]) el++;
        if (n > el) { int m = 1; for (int i = 0; i < el; i++) if (path[n-el+i] != e[k][i]) { m = 0; break; } if (m) return 1; } }
    return 0;
}
/* If `path` is AIPL, translate raw[0..rawlen) into g_xlat and point *csrc at
 * it (returns the C length); otherwise *csrc = raw (returns rawlen).  <0 on a
 * translation error (message already printed). */
static int aipl_prep(const char *path, char *raw, int rawlen, char **csrc)
{
    if (!src_is_aipl(path)) { *csrc = raw; return rawlen; }
    extern int         abcl2c(const char *, int, char *, int);
    extern const char *abcl2c_error(void);
    int r = abcl2c(raw, rawlen, g_xlat, sizeof g_xlat);
    if (r < 0) { uart_puts("abcl2c: "); uart_puts(abcl2c_error()); uart_putc('\n'); return -1; }
    *csrc = g_xlat; return r;
}

static int cc_compile_and_run(const char *path)
{
    extern const char *cc_last_error(void);
    char rp[128];
    fs_resolve(path, rp, sizeof rp);
    vfs_node_t *n = vfs_lookup(rp);
    if (!n || n->kind != VFS_FILE) {
        uart_puts("cc: no such file: "); uart_puts(rp); uart_putc('\n');
        uart_puts("    try: cc /home/hello.c   (or /home/PingPong.abcl, /home/RotateLine.abcl)\n");
        return 1;
    }
    static char src[8192];
    int len = vfs_read(n, src, sizeof src - 1);
    src[len] = 0;

    /* real AIPL (.abcl/.aipl) is translated to C by the on-device abcl2c */
    char *csrc; int clen = aipl_prep(rp, src, len, &csrc);
    if (clen < 0) return 1;

    uart_puts("cc: compiling + running "); uart_puts(rp);
    if (csrc != src) uart_puts(" (abcl2c -> C)");
    uart_puts(" (separate process)\n");

    /* Compile AND run in a dedicated process on a STATIC stack, so a buggy
     * program (deep recursion, runaway loop, bad pointer) can only blow that
     * stack and trip the runaway-loop deadline — never freeze the board.  A
     * static stack (not getmem) is mandatory: cc/make are dispatched from
     * genet_rx_tick (USB-keyboard pump + HTTP /run), where getmem() — which is
     * not reentrant against the main thread — would race and wedge the box. */
    static unsigned char ccrun_stack[CCRUN_STK] __attribute__((aligned(16)));
    g_ccrun_src = csrc; g_ccrun_len = clen;
    g_ccrun_out[0] = 0; g_ccrun_rv = 0; g_ccrun_rc = 0; g_ccrun_aborted = 0;
    int pid = proc_create_static(ccrun_proc_entry, ccrun_stack, CCRUN_STK, "cc-run");
    if (pid < 0) { uart_puts("cc: proc_create failed (no slot)\n"); return 1; }
    proc_resched();                 /* surrender CPU; returns after proc_exit() */

    if (g_ccrun_rc != 0) {
        uart_puts("cc: "); uart_puts(cc_last_error()); uart_putc('\n');
        return 1;
    }
    if (g_ccrun_out[0]) uart_puts(g_ccrun_out);
    if (g_ccrun_aborted)
        uart_puts("\ncc: aborted (ran past the runaway-loop deadline)\n");
    uart_puts("=> return value = "); puts_dec((int)g_ccrun_rv); uart_putc('\n');
    return 0;
}

static int cmd_cc(int argc, char **argv)
{
    if (argc < 2) {
        uart_puts("usage: cc <file>   compile a C/AIPL program and run it\n");
        return 1;
    }
    return cc_compile_and_run(argv[1]);
}

/* ============================================================ *
 *  make / run — build a Makefile + an executable-form file      *
 *                                                               *
 *  `make` no longer just aliases cc.  It (1) ensures a Makefile  *
 *  exists in the cwd (generating a default one and honouring its *
 *  SRC/TARGET), (2) compiles SRC with the on-device C compiler   *
 *  into an executable-form file TARGET (a saved machine-code     *
 *  image, see cc_compile_blob), and (3) runs it.  `run TARGET`   *
 *  re-executes a built file without recompiling.                 *
 * ============================================================ */
#define EXE_BLOB_CAP (192 * 1024)
static unsigned char g_exeblob[EXE_BLOB_CAP];
static unsigned char ccblob_stack[CCRUN_STK] __attribute__((aligned(16)));

/* compile SRC -> g_exeblob (own stack; cc recursion must not smash ours) */
static const char *g_mk_src; static int g_mk_srclen; static int g_mk_bloblen;
static void make_compile_proc(void)
{
    extern int cc_compile_blob(const char *, int, unsigned char *, int);
    g_mk_bloblen = cc_compile_blob(g_mk_src, g_mk_srclen, g_exeblob, sizeof g_exeblob);
    proc_exit();
}

/* run an executable-form blob (own stack + runaway-loop deadline) */
static const unsigned char *g_rb_blob; static int g_rb_len;
static char g_rb_out[2048]; static long g_rb_rv; static int g_rb_rc;
static void run_blob_proc(void)
{
    extern int cc_run_blob(const unsigned char *, int, char *, int, long *);
    g_rb_rc = cc_run_blob(g_rb_blob, g_rb_len, g_rb_out, sizeof g_rb_out, &g_rb_rv);
    proc_exit();
}

static int run_exe_blob(const unsigned char *blob, int len)
{
    g_rb_blob = blob; g_rb_len = len; g_rb_out[0] = 0; g_rb_rv = 0; g_rb_rc = 0;
    int pid = proc_create_static(run_blob_proc, ccblob_stack, CCRUN_STK, "run");
    if (pid < 0) { uart_puts("run: no proc slot\n"); return 1; }
    proc_resched();
    if (g_rb_rc != 0) {
        uart_puts("run: not an executable-form file "
                  "(bad magic / built by another kernel)\n");
        return 1;
    }
    if (g_rb_out[0]) uart_puts(g_rb_out);
    uart_puts("=> return value = "); puts_dec((int)g_rb_rv); uart_putc('\n');
    return 0;
}

/* small string append into a fixed buffer */
static int s_app(char *b, int p, int cap, const char *s)
{ while (*s && p < cap - 1) b[p++] = *s++; b[p] = 0; return p; }

/* base name of `src` minus a trailing .c/.abcl/.aipl -> out (the TARGET) */
static void make_target_name(const char *src, char *out, int outsz)
{
    const char *b = src; for (const char *p = src; *p; p++) if (*p == '/') b = p + 1;
    int n = 0; for (; b[n] && n < outsz - 1; n++) out[n] = b[n]; out[n] = 0;
    const char *ext[] = { ".c", ".abcl", ".aipl", 0 };
    for (int e = 0; ext[e]; e++) {
        int el = 0; while (ext[e][el]) el++;
        if (n > el) { int m = 1;
            for (int i = 0; i < el; i++) if (out[n-el+i] != ext[e][i]) { m = 0; break; }
            if (m) { out[n-el] = 0; return; } }
    }
}

/* read `key = value` from a Makefile buffer -> out; returns 1 if found */
static int mk_get(const char *buf, int len, const char *key, char *out, int outsz)
{
    int kl = 0; while (key[kl]) kl++;
    for (int i = 0; i < len; ) {
        int j = i; while (j < len && (buf[j]==' '||buf[j]=='\t')) j++;
        int m = 1; for (int k = 0; k < kl; k++) if (j+k >= len || buf[j+k] != key[k]) { m = 0; break; }
        if (m) { int p = j + kl;
            while (p < len && (buf[p]==' '||buf[p]=='\t')) p++;
            if (p < len && buf[p] == '=') { p++;
                while (p < len && (buf[p]==' '||buf[p]=='\t')) p++;
                int o = 0;
                while (p < len && buf[p]!='\n' && buf[p]!='\r' && buf[p]!=' ' && buf[p]!='\t' && o < outsz-1)
                    out[o++] = buf[p++];
                out[o] = 0; if (o > 0) return 1; } }
        while (i < len && buf[i] != '\n') i++; i++;
    }
    return 0;
}

/* open an existing regular file at an absolute path, else create it */
static vfs_node_t *fs_open_or_create(const char *abspath)
{
    vfs_node_t *n = vfs_lookup(abspath);
    if (n) return (n->kind == VFS_FILE) ? n : 0;
    return vfs_create_path(abspath);
}

static int cmd_make(int argc, char **argv)
{
    extern const char *cc_last_error(void);
    char src[64] = "hello.c", target[64] = "hello";
    char mkpath[160], srcpath[160], exepath[160];
    static char mkbuf[2048], srcbuf[4096];

    /* 1. Makefile in the cwd: read SRC/TARGET, or generate a default one. */
    fs_resolve("Makefile", mkpath, sizeof mkpath);
    vfs_node_t *mk = vfs_lookup(mkpath);
    int have_mk = (mk && mk->kind == VFS_FILE);
    if (have_mk) {
        int n = vfs_read(mk, mkbuf, sizeof mkbuf - 1); mkbuf[n] = 0;
        char v[64];
        if (mk_get(mkbuf, n, "SRC",    v, sizeof v)) { int i=0; for(;v[i]&&i<63;i++) src[i]=v[i];    src[i]=0; }
        if (mk_get(mkbuf, n, "TARGET", v, sizeof v)) { int i=0; for(;v[i]&&i<63;i++) target[i]=v[i]; target[i]=0; }
    }
    /* explicit arg wins and derives the target name */
    if (argc >= 2) { int i=0; for(;argv[1][i]&&i<63;i++) src[i]=argv[1][i]; src[i]=0;
                     make_target_name(src, target, sizeof target); }
    else if (!have_mk) make_target_name(src, target, sizeof target);

    /* 2. generate a Makefile when none exists */
    if (!have_mk) {
        vfs_node_t *f = fs_open_or_create(mkpath);
        if (f) {
            static char gen[512]; int p = 0;
            p = s_app(gen, p, sizeof gen,
                "# Auto-generated by `make` on Xinu Pi5 (on-device JIT toolchain).\n"
                "# `make` compiles SRC with the built-in C compiler into the\n"
                "# executable-form file TARGET (a saved machine-code image), then\n"
                "# runs it.  Re-run a built file without recompiling:  run TARGET\n"
                "SRC = ");
            p = s_app(gen, p, sizeof gen, src);
            p = s_app(gen, p, sizeof gen, "\nTARGET = ");
            p = s_app(gen, p, sizeof gen, target);
            p = s_app(gen, p, sizeof gen, "\n");
            vfs_write_str(f, gen);
            uart_puts("make: wrote "); uart_puts(mkpath); uart_putc('\n');
        }
    }

    /* 3. read the source */
    fs_resolve(src, srcpath, sizeof srcpath);
    vfs_node_t *sn = vfs_lookup(srcpath);
    if (!sn || sn->kind != VFS_FILE) {
        uart_puts("make: no such source: "); uart_puts(srcpath); uart_putc('\n');
        uart_puts("  (set SRC in the Makefile, or: make <file.c>)\n");
        return 1;
    }
    int slen = vfs_read(sn, srcbuf, sizeof srcbuf - 1); srcbuf[slen] = 0;

    /* real AIPL (.abcl/.aipl) is translated to C by the on-device abcl2c */
    char *csrc; int clen = aipl_prep(srcpath, srcbuf, slen, &csrc);
    if (clen < 0) return 1;
    if (csrc != srcbuf) { uart_puts("make: abcl2c "); uart_puts(srcpath); uart_puts(" -> C\n"); }

    /* 4. compile -> executable-form blob (own stack) */
    g_mk_src = csrc; g_mk_srclen = clen; g_mk_bloblen = -1;
    int pid = proc_create_static(make_compile_proc, ccblob_stack, CCRUN_STK, "cc-make");
    if (pid < 0) { uart_puts("make: no proc slot\n"); return 1; }
    proc_resched();
    if (g_mk_bloblen < 0) {
        uart_puts("make: compile failed: "); uart_puts(cc_last_error()); uart_putc('\n');
        return 1;
    }

    /* 5. write the executable-form file */
    fs_resolve(target, exepath, sizeof exepath);
    vfs_node_t *ef = fs_open_or_create(exepath);
    if (!ef) { uart_puts("make: cannot create "); uart_puts(exepath); uart_putc('\n'); return 1; }
    vfs_write(ef, g_exeblob, (unsigned long)g_mk_bloblen);
    uart_puts("make: "); uart_puts(srcpath); uart_puts(" -> "); uart_puts(exepath);
    uart_puts(" ("); puts_dec(g_mk_bloblen); uart_puts(" bytes)\n");

    /* 6. run it */
    uart_puts("make: running "); uart_puts(target); uart_putc('\n');
    return run_exe_blob(g_exeblob, g_mk_bloblen);
}

static int cmd_run(int argc, char **argv)
{
    if (argc < 2) {
        uart_puts("usage: run <file>   execute an executable-form file built by make\n");
        return 1;
    }
    char path[160]; fs_resolve(argv[1], path, sizeof path);
    vfs_node_t *n = vfs_lookup(path);
    if (!n || n->kind != VFS_FILE) {
        uart_puts("run: no such file: "); uart_puts(path); uart_putc('\n'); return 1;
    }
    int len = vfs_read(n, g_exeblob, sizeof g_exeblob);
    return run_exe_blob(g_exeblob, len);
}

/* ============================================================ *
 *  edit — a small emacs-style full-screen text editor          *
 *                                                              *
 *  Drives the SERIAL terminal with ANSI escapes (the HDMI/wm   *
 *  console is line-oriented, so use a serial terminal).  The   *
 *  network + window manager keep running from the timer IRQ    *
 *  while edit blocks on uart_getc(), so the box stays alive.   *
 *  Bindings (Emacs):                                           *
 *    C-f/C-b  fwd/back char     C-n/C-p  next/prev line        *
 *    C-a/C-e  start/end of line arrows too                     *
 *    C-d      delete char       Backspace delete back          *
 *    C-k      kill to EOL       Enter     split line           *
 *    C-x C-s  save              C-x C-c   quit                 *
 * ============================================================ */
#define ED_ROWS 256
#define ED_COLS 200
#define ED_VIEW 22                       /* visible text rows (status on 23) */
static char ed_text[ED_ROWS][ED_COLS];
static int  ed_llen[ED_ROWS];
static int  ed_n, ed_cy, ed_cx, ed_top, ed_dirty;

static void ed_num(int v)
{ char b[8]; int n = 0; if (v <= 0) { uart_putc('0'); return; }
  while (v) { b[n++] = (char)('0' + v % 10); v /= 10; } while (n) uart_putc(b[--n]); }

static void ed_gotoxy(int row, int col)            /* 1-based */
{ uart_puts("\x1b["); ed_num(row); uart_putc(';'); ed_num(col); uart_putc('H'); }

static void ed_place_cursor(void)
{ ed_gotoxy(ed_cy - ed_top + 1, ed_cx + 1); }

static void ed_draw_status(const char *fn)
{
    ed_gotoxy(ED_VIEW + 1, 1);
    uart_puts("\x1b[7m\x1b[K");                     /* reverse video + clear  */
    uart_puts(" edit "); uart_puts(fn);
    uart_puts(ed_dirty ? "  [modified] " : "  [saved] ");
    uart_puts(" C-x C-s save  C-x C-c quit   ln ");
    ed_num(ed_cy + 1); uart_putc('/'); ed_num(ed_n);
    uart_puts("\x1b[0m");
}

static void ed_draw_textrow(int sr)                 /* sr = 0..ED_VIEW-1      */
{
    int y = ed_top + sr;
    ed_gotoxy(sr + 1, 1); uart_puts("\x1b[K");
    if (y < ed_n) for (int i = 0; i < ed_llen[y]; i++) uart_putc(ed_text[y][i]);
}

static void ed_redraw(const char *fn)
{
    uart_puts("\x1b[2J");
    for (int sr = 0; sr < ED_VIEW; sr++) ed_draw_textrow(sr);
    ed_draw_status(fn);
    ed_place_cursor();
}

/* keep the cursor row on screen; returns 1 if the view scrolled */
static int ed_scroll(void)
{
    int old = ed_top;
    if (ed_cy < ed_top)              ed_top = ed_cy;
    if (ed_cy >= ed_top + ED_VIEW)   ed_top = ed_cy - ED_VIEW + 1;
    if (ed_top < 0) ed_top = 0;
    return ed_top != old;
}

static void ed_save(const char *path, const char *fn)
{
    static char ob[ED_ROWS * ED_COLS]; int p = 0;
    for (int y = 0; y < ed_n; y++) {
        for (int i = 0; i < ed_llen[y] && p < (int)sizeof ob - 2; i++) ob[p++] = ed_text[y][i];
        if (y < ed_n - 1 && p < (int)sizeof ob - 2) ob[p++] = '\n';
    }
    ob[p] = 0;
    vfs_node_t *f = fs_open_or_create(path);
    if (f) { vfs_write(f, ob, (unsigned long)p); ed_dirty = 0; }
    ed_draw_status(fn); ed_place_cursor();
}

static int cmd_edit(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: edit <file>   emacs-style editor (serial terminal)\n"); return 1; }
    char path[160]; fs_resolve(argv[1], path, sizeof path);

    /* load (or start empty) */
    ed_n = 0; ed_cy = ed_cx = ed_top = ed_dirty = 0;
    vfs_node_t *node = vfs_lookup(path);
    if (node && node->kind == VFS_FILE) {
        static char fb[ED_ROWS * ED_COLS];
        int len = vfs_read(node, fb, sizeof fb - 1); if (len < 0) len = 0; fb[len] = 0;
        int row = 0, col = 0;
        for (int i = 0; i < len && row < ED_ROWS; i++) {
            char ch = fb[i];
            if (ch == '\r') continue;
            if (ch == '\n') { ed_llen[row] = col; row++; col = 0; }
            else if (col < ED_COLS - 1) ed_text[row][col++] = ch;
        }
        if (row < ED_ROWS) { ed_llen[row] = col; row++; }
        ed_n = row;
    } else if (node) {
        uart_puts("edit: not a file: "); uart_puts(path); uart_putc('\n'); return 1;
    }
    if (ed_n == 0) { ed_llen[0] = 0; ed_n = 1; }

    ed_redraw(argv[1]);

    int prefix = 0;                                 /* C-x seen */
    for (;;) {
        char c = uart_getc();

        if (prefix) {
            prefix = 0;
            if (c == 0x13) { ed_save(path, argv[1]); continue; }   /* C-s */
            if (c == 0x03) break;                                  /* C-c */
            continue;
        }
        if (c == 0x18) { prefix = 1; continue; }                   /* C-x */

        /* arrow keys: ESC [ A/B/C/D -> map to C-p/n/f/b */
        if (c == 0x1b) {
            char b1 = uart_getc();
            if (b1 == '[') { char d = uart_getc();
                if (d == 'A') c = 0x10; else if (d == 'B') c = 0x0e;
                else if (d == 'C') c = 0x06; else if (d == 'D') c = 0x02;
                else continue;
            } else continue;
        }

        if (ed_cx > ed_llen[ed_cy]) ed_cx = ed_llen[ed_cy];

        if (c == 0x06) {                                            /* C-f */
            if (ed_cx < ed_llen[ed_cy]) ed_cx++;
            else if (ed_cy < ed_n - 1) { ed_cy++; ed_cx = 0; }
        } else if (c == 0x02) {                                     /* C-b */
            if (ed_cx > 0) ed_cx--;
            else if (ed_cy > 0) { ed_cy--; ed_cx = ed_llen[ed_cy]; }
        } else if (c == 0x0e) {                                     /* C-n */
            if (ed_cy < ed_n - 1) { ed_cy++; if (ed_cx > ed_llen[ed_cy]) ed_cx = ed_llen[ed_cy]; }
        } else if (c == 0x10) {                                     /* C-p */
            if (ed_cy > 0) { ed_cy--; if (ed_cx > ed_llen[ed_cy]) ed_cx = ed_llen[ed_cy]; }
        } else if (c == 0x01) { ed_cx = 0; }                        /* C-a */
        else if (c == 0x05) { ed_cx = ed_llen[ed_cy]; }             /* C-e */
        else if (c == 0x0b) {                                       /* C-k kill EOL */
            ed_llen[ed_cy] = ed_cx; ed_dirty = 1;
            ed_draw_textrow(ed_cy - ed_top); ed_draw_status(argv[1]); ed_place_cursor();
            continue;
        } else if (c == 0x7f || c == 0x08) {                        /* Backspace */
            if (ed_cx > 0) {
                for (int i = ed_cx - 1; i < ed_llen[ed_cy] - 1; i++) ed_text[ed_cy][i] = ed_text[ed_cy][i + 1];
                ed_llen[ed_cy]--; ed_cx--; ed_dirty = 1;
                ed_draw_textrow(ed_cy - ed_top); ed_draw_status(argv[1]); ed_place_cursor();
                continue;
            } else if (ed_cy > 0) {                                 /* join with previous */
                int prev = ed_cy - 1, pl = ed_llen[prev];
                for (int i = 0; i < ed_llen[ed_cy] && pl + i < ED_COLS - 1; i++) ed_text[prev][pl + i] = ed_text[ed_cy][i];
                ed_llen[prev] = pl + ed_llen[ed_cy];
                for (int y = ed_cy; y < ed_n - 1; y++) { for (int i = 0; i < ed_llen[y + 1]; i++) ed_text[y][i] = ed_text[y + 1][i]; ed_llen[y] = ed_llen[y + 1]; }
                ed_n--; ed_cy = prev; ed_cx = pl; ed_dirty = 1;
                ed_scroll(); ed_redraw(argv[1]); continue;
            }
        } else if (c == 0x04) {                                     /* C-d delete fwd */
            if (ed_cx < ed_llen[ed_cy]) {
                for (int i = ed_cx; i < ed_llen[ed_cy] - 1; i++) ed_text[ed_cy][i] = ed_text[ed_cy][i + 1];
                ed_llen[ed_cy]--; ed_dirty = 1;
                ed_draw_textrow(ed_cy - ed_top); ed_draw_status(argv[1]); ed_place_cursor();
                continue;
            } else if (ed_cy < ed_n - 1) {                          /* join next up */
                int nl = ed_llen[ed_cy + 1];
                for (int i = 0; i < nl && ed_cx + i < ED_COLS - 1; i++) ed_text[ed_cy][ed_cx + i] = ed_text[ed_cy + 1][i];
                ed_llen[ed_cy] = ed_cx + nl;
                for (int y = ed_cy + 1; y < ed_n - 1; y++) { for (int i = 0; i < ed_llen[y + 1]; i++) ed_text[y][i] = ed_text[y + 1][i]; ed_llen[y] = ed_llen[y + 1]; }
                ed_n--; ed_dirty = 1; ed_redraw(argv[1]); continue;
            }
        } else if (c == 0x0d || c == 0x0a) {                        /* Enter: split */
            if (ed_n < ED_ROWS) {
                for (int y = ed_n; y > ed_cy + 1; y--) { for (int i = 0; i < ed_llen[y - 1]; i++) ed_text[y][i] = ed_text[y - 1][i]; ed_llen[y] = ed_llen[y - 1]; }
                int tail = ed_llen[ed_cy] - ed_cx;
                for (int i = 0; i < tail; i++) ed_text[ed_cy + 1][i] = ed_text[ed_cy][ed_cx + i];
                ed_llen[ed_cy + 1] = tail; ed_llen[ed_cy] = ed_cx;
                ed_n++; ed_cy++; ed_cx = 0; ed_dirty = 1;
                ed_scroll(); ed_redraw(argv[1]); continue;
            }
        } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {  /* insert */
            if (ed_llen[ed_cy] < ED_COLS - 1) {
                for (int i = ed_llen[ed_cy]; i > ed_cx; i--) ed_text[ed_cy][i] = ed_text[ed_cy][i - 1];
                ed_text[ed_cy][ed_cx] = c; ed_llen[ed_cy]++; ed_cx++; ed_dirty = 1;
                ed_draw_textrow(ed_cy - ed_top); ed_draw_status(argv[1]); ed_place_cursor();
                continue;
            }
        }

        /* cursor-move fallthrough: redraw if it scrolled, else just move */
        if (ed_scroll()) ed_redraw(argv[1]);
        else { ed_draw_status(argv[1]); ed_place_cursor(); }
    }

    uart_puts("\x1b[2J\x1b[H");                     /* clean exit */
    uart_puts("edit: done");
    if (ed_dirty) uart_puts(" (unsaved changes discarded — C-x C-s to save)");
    uart_putc('\n');
    return 0;
}

/* abcl2c <file.abcl> — translate real AIPL to C and write <file>.c.
 * (cc/make on a .abcl already translate transparently; this exposes the C.) */
static int cmd_abcl2c(int argc, char **argv)
{
    if (argc < 2) { uart_puts("usage: abcl2c <file.abcl>   (writes <file>.c)\n"); return 1; }
    char path[160]; fs_resolve(argv[1], path, sizeof path);
    vfs_node_t *n = vfs_lookup(path);
    if (!n || n->kind != VFS_FILE) { uart_puts("abcl2c: no such file: "); uart_puts(path); uart_putc('\n'); return 1; }
    static char raw[8192]; int rl = vfs_read(n, raw, sizeof raw - 1); raw[rl] = 0;

    extern int         abcl2c(const char *, int, char *, int);
    extern const char *abcl2c_error(void);
    int r = abcl2c(raw, rl, g_xlat, sizeof g_xlat);
    if (r < 0) { uart_puts("abcl2c: "); uart_puts(abcl2c_error()); uart_putc('\n'); return 1; }

    /* output path: strip .abcl/.aipl, append .c */
    char op[160]; int i = 0; for (; path[i] && i < 150; i++) op[i] = path[i]; op[i] = 0;
    int n2 = i; const char *ex[] = { ".abcl", ".aipl", 0 };
    for (int k = 0; ex[k]; k++) { int el = 0; while (ex[k][el]) el++;
        if (n2 > el) { int m = 1; for (int j = 0; j < el; j++) if (op[n2-el+j] != ex[k][j]) { m = 0; break; } if (m) { n2 -= el; break; } } }
    op[n2] = '.'; op[n2+1] = 'c'; op[n2+2] = 0;

    vfs_node_t *f = fs_open_or_create(op);
    if (!f) { uart_puts("abcl2c: cannot create "); uart_puts(op); uart_putc('\n'); return 1; }
    vfs_write(f, g_xlat, (unsigned long)r);
    uart_puts("abcl2c: "); uart_puts(path); uart_puts(" -> "); uart_puts(op);
    uart_puts(" ("); puts_dec(r); uart_puts(" bytes C)\n  run it:  cc "); uart_puts(op); uart_putc('\n');
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    extern void shellwin_clear(void);
    (void)argc; (void)argv;
    shellwin_clear();           /* wipe scrollback; the post-dispatch prompt
                                 * then lands on the now-empty first line */
    return 0;
}

static int cmd_wine(int argc, char **argv)
{
    extern void graphics_wine_start(void);
    (void)argc; (void)argv;
    graphics_wine_start();
    uart_puts("wine: spinning the 3D wireframe wine glass in the Graphics window\n");
    return 0;
}

static int cmd_4lines(int argc, char **argv)
{
    extern void graphics_4lines_start(void);
    (void)argc; (void)argv;
    graphics_4lines_start();
    uart_puts("4lines: spinning 4 segments centred on a square's corners\n");
    return 0;
}

static int cmd_kodama(int argc, char **argv)
{
    extern void graphics_kodama_start(void);
    (void)argc; (void)argv;
    graphics_kodama_start();
    uart_puts("kodama: spinning 3D block text in the Graphics window\n");
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        uart_puts(argv[i]);
        if (i + 1 < argc) uart_putc(' ');
    }
    uart_puts("\n");
    return 0;
}

static int cmd_hello(int argc, char **argv)
{
    (void)argc; (void)argv;
    uart_puts("hello from Xinu on Raspberry Pi 5 (BCM2712, AArch64)\n");
    return 0;
}

static int cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    uart_puts("__bss_start = "); puts_hex((unsigned long)&__bss_start); uart_puts("\n");
    uart_puts("__bss_end   = "); puts_hex((unsigned long)&__bss_end);   uart_puts("\n");
    uart_puts("_end        = "); puts_hex((unsigned long)&_end);        uart_puts("\n");

    /* Heap stats from the first-fit allocator. */
    uart_puts("\nheap:\n");
    uart_puts("  total      = "); puts_dec((int)(mem_total_bytes() >> 10));
    uart_puts(" KiB\n");
    uart_puts("  free       = "); puts_dec((int)(mem_free_bytes() >> 10));
    uart_puts(" KiB  ("); puts_dec((int)mem_free_bytes()); uart_puts(" bytes)\n");
    uart_puts("  largest    = "); puts_dec((int)mem_largest_block());
    uart_puts(" bytes\n");
    uart_puts("  free blocks= "); puts_dec(mem_free_block_count());
    uart_puts("\n");
    return 0;
}

static int cmd_peek(int argc, char **argv)
{
    if (argc < 2) {
        uart_puts("usage: peek <hex_addr>\n");
        return 1;
    }
    unsigned long addr = parse_hex(argv[1]);
    if (addr == 0) {
        uart_puts("peek: bad address\n");
        return 1;
    }
    unsigned int v = *(volatile unsigned int *)addr;
    uart_puts("["); puts_hex(addr); uart_puts("] = ");
    puts_hex((unsigned long)v);
    uart_puts("\n");
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* Generic timer plumbing arrives in phase S1.  For now read
     * CNTPCT_EL0 directly so the user gets *some* signal of life. */
    unsigned long cnt;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(cnt));
    uart_puts("cntpct_el0 = "); puts_hex(cnt); uart_puts("\n");
    return 0;
}

/* ---------- pingpong (cooperative 2-actor demo) ---------- */
/*
 * Stand-in for an AIPL Ping/Pong actor pair until phase S0 brings up
 * the real scheduler.  Each actor owns a single-slot inbox and a tiny
 * step() that consumes a message, logs it, and (unless its send budget
 * is exhausted) posts a reply to the peer's inbox.  The dispatcher is
 * the outer while loop in cmd_pingpong — it picks whichever inbox is
 * non-empty and runs that actor's step.  No threads, no preemption,
 * but the visible behaviour matches what AIPL emits on a real runtime.
 */

#define PP_DEFAULT_ROUNDS  5
#define PP_MAX_ROUNDS      50
#define PP_INBOX_LEN       32

typedef struct pp_actor {
    const char *name;        /* "Ping" / "Pong" */
    const char *outgoing;    /* the word this actor always sends */
    char        inbox[PP_INBOX_LEN];
    int         has_msg;
    int         sent;
    int         recv;
} pp_actor_t;

static pp_actor_t pp_ping = { "Ping", "ping", {0}, 0, 0, 0 };
static pp_actor_t pp_pong = { "Pong", "pong", {0}, 0, 0, 0 };

static void pp_copy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void pp_post(pp_actor_t *dest, const char *msg)
{
    pp_copy(dest->inbox, msg, PP_INBOX_LEN);
    dest->has_msg = 1;
}

static void pp_step(pp_actor_t *self, pp_actor_t *peer, int max_sends)
{
    self->recv++;
    uart_puts("  ["); uart_puts(self->name);
    uart_puts("] recv '"); uart_puts(self->inbox);
    uart_puts("' (msg #"); puts_dec(self->recv); uart_puts(")\n");
    self->has_msg = 0;

    if (self->sent >= max_sends) {
        uart_puts("  ["); uart_puts(self->name);
        uart_puts("] budget exhausted (sent ");
        puts_dec(self->sent); uart_puts(") — no reply\n");
        return;
    }

    pp_post(peer, self->outgoing);
    self->sent++;
    uart_puts("  ["); uart_puts(self->name);
    uart_puts("] send '"); uart_puts(self->outgoing);
    uart_puts("' -> "); uart_puts(peer->name); uart_puts("\n");
}

static int cmd_pingpong(int argc, char **argv)
{
    int rounds = PP_DEFAULT_ROUNDS;

    if (argc >= 2) {
        int v = 0;
        const char *s = argv[1];
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
        if (*s != 0 || v < 1) {
            uart_puts("usage: pingpong [N]   (1..");
            puts_dec(PP_MAX_ROUNDS); uart_puts(", default ");
            puts_dec(PP_DEFAULT_ROUNDS); uart_puts(")\n");
            return 1;
        }
        if (v > PP_MAX_ROUNDS) v = PP_MAX_ROUNDS;
        rounds = v;
    }

    /* Reset actor state — safe to re-run the command. */
    pp_ping.has_msg = 0; pp_ping.sent = 0; pp_ping.recv = 0;
    pp_pong.has_msg = 0; pp_pong.sent = 0; pp_pong.recv = 0;

    uart_puts("pingpong: spawning Ping + Pong, rounds=");
    puts_dec(rounds); uart_puts("\n");
    uart_puts("---------------------------------------------\n");

    /* Bootstrap: Ping sends the first message itself. */
    pp_post(&pp_pong, pp_ping.outgoing);
    pp_ping.sent = 1;
    uart_puts("  [Ping] send 'ping' -> Pong   (bootstrap)\n");

    /* Cooperative dispatcher: drain whichever inbox has a message. */
    while (pp_ping.has_msg || pp_pong.has_msg) {
        if (pp_pong.has_msg) pp_step(&pp_pong, &pp_ping, rounds);
        else if (pp_ping.has_msg) pp_step(&pp_ping, &pp_pong, rounds);
    }

    uart_puts("---------------------------------------------\n");
    uart_puts("pingpong: done.  Ping  sent=");
    puts_dec(pp_ping.sent); uart_puts(" recv=");
    puts_dec(pp_ping.recv); uart_puts("\n");
    uart_puts("                 Pong  sent=");
    puts_dec(pp_pong.sent); uart_puts(" recv=");
    puts_dec(pp_pong.recv); uart_puts("\n");
    return 0;
}

/* ---------- procdemo (real ctxsw, 2 processes) ---------- */
/*
 * Ground-truth demo for the S0 scheduler: two genuine processes
 * (each with its own getmem'd stack) that yield to each other in a
 * tight loop.  Unlike `pingpong` — which simulates message passing
 * inside one C call stack — these two functions run on independent
 * SPs and only resume each other through ctxsw.S.
 *
 * Each process logs the current pid (read live from `currpid`) so
 * you can see the scheduler flipping control between them in the
 * output stream.  When both reach the end of their loop they call
 * proc_exit(), which drops them from the ready list; the dispatcher
 * then ctxsw'es back to NULLPROC (the shell) and cmd_procdemo
 * returns from proc_resched().
 */

#define PROCDEMO_DEFAULT_ITERS  5
#define PROCDEMO_MAX_ITERS      30
#define PROCDEMO_STK            4096

static int procdemo_iters;

static void procdemo_log(const char *who, const char *verb, int i)
{
    uart_puts("  ["); uart_puts(who);
    uart_puts(" pid="); puts_dec(currpid);
    uart_puts("] "); uart_puts(verb);
    uart_puts(" "); puts_dec(i); uart_puts("\n");
}

static void procdemo_ping_entry(void)
{
    int i;
    for (i = 1; i <= procdemo_iters; i++) {
        procdemo_log("Ping", "tick", i);
        proc_yield();
    }
    procdemo_log("Ping", "exit at iter", procdemo_iters);
    proc_exit();
}

static void procdemo_pong_entry(void)
{
    int i;
    for (i = 1; i <= procdemo_iters; i++) {
        procdemo_log("Pong", "tock", i);
        proc_yield();
    }
    procdemo_log("Pong", "exit at iter", procdemo_iters);
    proc_exit();
}

static int cmd_procdemo(int argc, char **argv)
{
    int iters = PROCDEMO_DEFAULT_ITERS;
    int p, q;

    if (argc >= 2) {
        int v = 0;
        const char *s = argv[1];
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
        if (*s != 0 || v < 1) {
            uart_puts("usage: procdemo [N]   (1..");
            puts_dec(PROCDEMO_MAX_ITERS); uart_puts(", default ");
            puts_dec(PROCDEMO_DEFAULT_ITERS); uart_puts(")\n");
            return 1;
        }
        if (v > PROCDEMO_MAX_ITERS) v = PROCDEMO_MAX_ITERS;
        iters = v;
    }
    procdemo_iters = iters;

    p = proc_create(procdemo_ping_entry, PROCDEMO_STK, "ping-proc");
    if (p < 0) {
        uart_puts("procdemo: proc_create(ping) failed\n");
        return 1;
    }
    q = proc_create(procdemo_pong_entry, PROCDEMO_STK, "pong-proc");
    if (q < 0) {
        uart_puts("procdemo: proc_create(pong) failed\n");
        proctab[p].state = PR_FREE;
        return 1;
    }

    uart_puts("procdemo: created pid="); puts_dec(p);
    uart_puts(" (ping) and pid=");      puts_dec(q);
    uart_puts(" (pong), iters=");       puts_dec(iters);
    uart_puts("\n---------------------------------------------\n");

    /* Surrender the CPU to the ready list.  Returns here only after
     * both children have called proc_exit() and the dispatcher has
     * ctxsw'ed back to NULLPROC. */
    proc_resched();

    uart_puts("---------------------------------------------\n");
    uart_puts("procdemo: both processes exited; back in shell.\n");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    /* Reset the SoC via the BCM2712 PM watchdog ("brcm,bcm2712-pm") at CPU PA
     * 0x10_7D200000 — the same bcm2835-layout PM block (PM_RSTC 0x1c, PM_RSTS
     * 0x20, PM_WDOG 0x24, password 0x5a000000) the Pi firmware uses to restart.
     * Arm a ~150us watchdog with WRCFG_FULL_RESET; the hardware then resets. */
    volatile unsigned int *PM = (volatile unsigned int *)0x107D200000UL;
    const unsigned int PW = 0x5a000000u;          /* PM_PASSWORD */
    volatile unsigned long d; unsigned int v;
    (void)argc; (void)argv;
    uart_puts("reboot: BCM2712 PM watchdog -> full reset...\n");
    v = (PM[0x20/4] & 0xfffffaaau) | PW;           /* PM_RSTS -> boot partition 0 */
    PM[0x20/4] = v;
    PM[0x24/4] = 10u | PW;                          /* PM_WDOG: ~10 ticks (~150us)  */
    v = (PM[0x1c/4] & 0xffffffcfu) | PW | 0x20u;    /* PM_RSTC: WRCFG_FULL_RESET     */
    PM[0x1c/4] = v;
    for (d = 0; d < 200000000UL; d++) __asm__ volatile ("nop");   /* wait for it */
    uart_puts("reboot: watchdog did not fire (firmware may block it).\n");
    for (;;) __asm__ volatile ("wfe");
}

static int cmd_ps(int argc, char **argv)
{
    unsigned long mpidr, midr, current_el;
    int i;
    (void)argc; (void)argv;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile ("mrs %0, midr_el1"  : "=r"(midr));
    __asm__ volatile ("mrs %0, currentel" : "=r"(current_el));
    current_el = (current_el >> 2) & 3;

    /* Top section: per-core CPU state.  boot.S parks cores 1-3 in
     * WFE; only core 0 ever runs.  Phase S1 + GIC IPIs would change
     * that. */
    uart_puts("Cores:\n");
    uart_puts(" CORE  STATE      EL  MIDR_EL1\n");
    uart_puts("    ");
    uart_putc((char)('0' + (mpidr & 0xFF)));
    uart_puts("  RUN        ");
    uart_putc((char)('0' + current_el));
    uart_puts("   "); puts_hex(midr); uart_puts("\n");
    for (i = 1; i < 4; i++) {
        uart_puts("    "); uart_putc((char)('0' + i));
        uart_puts("  PARK(WFE)  -   -\n");
    }

    /* Bottom section: real proctab.  PR_FREE rows are skipped except
     * for slot 0 which is always reserved for the null/shell context. */
    uart_puts("\nProcess table:\n");
    uart_puts(" PID  STATE  PRIO  STACK_BASE          NAME\n");
    for (i = 0; i < NPROC; i++) {
        const char *st;
        if (proctab[i].state == PR_FREE && i != NULLPROC) continue;
        switch (proctab[i].state) {
            case PR_CURR:  st = "CURR "; break;
            case PR_READY: st = "READY"; break;
            case PR_FREE:  st = "FREE "; break;
            case PR_TERM:  st = "TERM "; break;
            default:       st = "?    "; break;
        }
        uart_puts("  "); uart_putc((char)('0' + i));
        uart_puts("  "); uart_puts(st);
        uart_puts("    "); puts_dec(proctab[i].prio);
        uart_puts("     "); puts_hex((unsigned long)proctab[i].stkbase);
        uart_puts("  "); uart_puts(proctab[i].name);
        if (i == currpid) uart_puts("  <-- running");
        uart_puts("\n");
    }
    return 0;
}

static int cmd_rxstat(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* Drain pending RX frames first, so packet count reflects HW
     * state at the moment of the command rather than just what
     * background polling collected. */
    unsigned char *pkt;
    int len;
    int drained = 0;
    while ((len = genet_rx_poll(&pkt)) > 0) {
        genet_rx_release();
        drained++;
        if (drained >= 32) break;
    }
    uart_puts("rxstat: packets=");
    {
        unsigned long v = genet_rx_packet_count();
        char buf[24]; int n = 0;
        if (v == 0) uart_putc('0');
        else { while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
               while (n--) uart_putc(buf[n]); }
    }
    uart_puts(" bytes=");
    {
        unsigned long v = genet_rx_byte_count();
        char buf[24]; int n = 0;
        if (v == 0) uart_putc('0');
        else { while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
               while (n--) uart_putc(buf[n]); }
    }
    uart_puts("  drained="); puts_dec(drained); uart_puts("\n");
    /* NET-G health counters: overruns/recoveries should stay 0 in
     * steady state.  A climbing tx_timeout means the TX ring is
     * degrading (and self-recovering) — useful when bisecting the
     * old "ICMP responder stopped" symptom under TCP load. */
    uart_puts("        overruns="); puts_dec((int)genet_rx_overrun_count());
    uart_puts(" recoveries=");      puts_dec((int)genet_rx_recover_count());
    uart_puts(" tx_timeouts=");     puts_dec((int)genet_tx_timeout_count());
    uart_puts("\n");
    return 0;
}

/* NET-G: show the TCP listener state + handshake counters on demand,
 * so a failed `nc` can be diagnosed without watching the boot log. */
extern void tcp_dump_state(void);
static int cmd_tcpstat(int argc, char **argv)
{
    (void)argc; (void)argv;
    tcp_dump_state();
    return 0;
}

static int cmd_pan(int argc, char **argv)
{
    if (argc < 3) {
        uart_puts("usage: pan <dx> <dy>  (signed decimal pixels)\n");
        uart_puts("       e.g. `pan 320 0` scrolls one viewport-width right\n");
        return 0;
    }
    /* tiny signed-decimal parser */
    int dx = 0, dy = 0;
    int neg = 0;
    const char *p = argv[1];
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { dx = dx * 10 + (*p - '0'); p++; }
    if (neg) dx = -dx;
    neg = 0; p = argv[2];
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { dy = dy * 10 + (*p - '0'); p++; }
    if (neg) dy = -dy;
    wm_set_autopan(0);
    wm_pan(dx, dy);
    uart_puts("pan: viewport now ("); puts_dec(wm_view_x());
    uart_puts(", "); puts_dec(wm_view_y()); uart_puts(")\n");
    return 0;
}

static int cmd_view(int argc, char **argv)
{
    (void)argc; (void)argv;
    uart_puts("viewport: ("); puts_dec(wm_view_x()); uart_puts(", ");
    puts_dec(wm_view_y()); uart_puts(")  desktop 1280x960  screen 640x480\n");
    return 0;
}

static int cmd_autopan(int argc, char **argv)
{
    int on = 1;
    if (argc >= 2 && (argv[1][0] == '0' || argv[1][0] == 'f')) on = 0;
    wm_set_autopan(on);
    uart_puts("autopan: "); uart_puts(on ? "on\n" : "off\n");
    return 0;
}

static int cmd_ticks(int argc, char **argv)
{
    (void)argc; (void)argv;
    unsigned long t = timer_ticks();
    uart_puts("ticks: ");
    puts_hex((unsigned long)t);
    uart_puts("  (");
    /* dec */
    if (t == 0) uart_putc('0');
    else {
        char buf[24]; int n = 0;
        while (t > 0) { buf[n++] = (char)('0' + (t % 10)); t /= 10; }
        while (n--) uart_putc(buf[n]);
    }
    uart_puts(" @ 100 Hz)\n");
    return 0;
}

static int cmd_usb(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!usb_present()) {
        uart_puts("usb: no DWC2 USB controller on this board\n");
        uart_puts("     (Pi 5 routes USB through RP1; QEMU virt has no DWC2)\n");
        return 0;
    }
    unsigned int id = usb_synopsys_id();
    uart_puts("DWC2 GSNPSID = ");
    puts_hex((unsigned long)id);
    uart_puts("\n");
    if ((id & 0xFFFF0000u) == 0x4F540000u) {
        uart_puts("  signature : OK (Synopsys 'OT' = 0x4F54)\n");
        uart_puts("  core rel  : 0x");
        for (int i = 3; i >= 0; i--) {
            unsigned int nyb = (id >> (i * 4)) & 0xF;
            uart_putc((char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10)));
        }
        uart_puts("\n");
        uart_puts("  init      : ");
        uart_puts(usb_last_init_ok() ? "ok\n" : "saw mismatch at boot\n");
    } else {
        uart_puts("  signature : MISMATCH — controller absent or unpowered\n");
        uart_puts("              (expected upper 16 bits == 0x4F54)\n");
    }
    return 0;
}

static int cmd_halt(int argc, char **argv)
{
    (void)argc; (void)argv;

    uart_puts("halt: masking DAIF, requesting PSCI SYSTEM_OFF...\n");

    /* Mask debug / SError / IRQ / FIQ so nothing wakes us up after
     * the WFE fallback below. */
    __asm__ volatile ("msr daifset, #0xf" ::: "memory");

    /* PSCI v0.2+ SYSTEM_OFF: function id 0x84000008, no arguments.
     *
     * QEMU virt's PSCI conduit is HVC even when the guest is at EL1
     * (the emulator catches the call regardless of EL3 presence).
     * Real Pi 5 firmware may or may not implement PSCI — we fall
     * through to WFE if HVC returns at all.
     *
     * We can't try SMC as a fallback first: with no exception
     * vectors installed an unrouted SMC at EL1 traps recursively
     * to the same EL and we'd execute garbage at the vector base. */
    register unsigned long x0 __asm__("x0") = 0x84000008UL;
    __asm__ volatile ("hvc #0" : "+r"(x0) :: "x1","x2","x3","memory");

    uart_puts("halt: PSCI returned — WFE forever.\n");
    for (;;) __asm__ volatile ("wfe");
}

/* ---------- WiFi: on / off / status / scan + wifi-invest ---------- */
static char g_wifi_ssid[64];
static char g_wifi_pass[80];
static int  g_wifi_have_creds = 0;

static void str_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
/* Remember WiFi creds (RAM only) so `wifi on` (no args) reconnects later — used
 * by the boot auto-connect so a subsequent `wifi off`/`wifi on` works too. */
void wifi_remember_creds(const char *ssid, const char *pass)
{
    str_copy(g_wifi_ssid, ssid, sizeof g_wifi_ssid);
    str_copy(g_wifi_pass, pass, sizeof g_wifi_pass);
    g_wifi_have_creds = 1;
}
static void puts_ip(const unsigned char *ip)
{
    int i;
    for (i = 0; i < 4; i++) { if (i) uart_putc('.'); puts_dec(ip[i]); }
}

/* wifi on [ssid pass] | wifi off | wifi status | wifi scan */
static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || str_eq(argv[1], "status")) {
        if (wifi_connected()) {
            unsigned char ip[4]; wifi_ipaddr(ip);
            uart_puts("wifi: connected  ssid=\""); uart_puts(wifi_ssid());
            uart_puts("\"  ip="); puts_ip(ip); uart_putc('\n');
        } else {
            uart_puts("wifi: not connected\n");
            if (argc < 2)
                uart_puts("usage: wifi on <ssid> <pass> | on | off | scan | status | adhoc <ssid> [ch] [node] | aodv <ip>\n");
        }
        return 0;
    }
    if (str_eq(argv[1], "off")) {
        wifi_off();
        uart_puts("wifi: off (radio down)\n");
        return 0;
    }
    if (str_eq(argv[1], "scan")) {
        int n;
        uart_puts("wifi: bringing up radio + scanning...\n");
        if (wifi_probe() != 0) { uart_puts("wifi: bring-up FAILED — run wifi-invest\n"); return 1; }
        n = wifi_scan_run();
        uart_puts("wifi: scan done — "); puts_dec(n);
        uart_puts(" APs (full list: wifi-invest)\n");
        return 0;
    }
    if (str_eq(argv[1], "on")) {
        static char cs[64], cp[80];      /* creds parsed from the embedded conf */
        const char *ssid = 0, *pass = 0;
        if (argc >= 4) {                 /* wifi on <ssid> <pass>: remember + use */
            ssid = argv[2]; pass = argv[3];
            str_copy(g_wifi_ssid, ssid, sizeof g_wifi_ssid);
            str_copy(g_wifi_pass, pass, sizeof g_wifi_pass);
            g_wifi_have_creds = 1;
        } else if (g_wifi_have_creds) {  /* wifi on: reuse last creds (RAM) */
            ssid = g_wifi_ssid; pass = g_wifi_pass;
        } else {                         /* wifi on: pull saved creds from wifi.conf */
            extern const char wifi_conf[], wifi_conf_end[];
            const char *c = wifi_conf; int avail = (int)(wifi_conf_end - wifi_conf), i = 0, j = 0;
            while (i < avail && c[i] && c[i] != '\n' && c[i] != '\r' && j < (int)sizeof cs - 1) cs[j++] = c[i++];
            cs[j] = 0;
            while (i < avail && (c[i] == '\n' || c[i] == '\r' || c[i] == ' ')) i++;
            j = 0;
            while (i < avail && c[i] && c[i] != '\n' && c[i] != '\r' && j < (int)sizeof cp - 1) cp[j++] = c[i++];
            cp[j] = 0;
            if (cs[0]) { ssid = cs; pass = cp; wifi_remember_creds(cs, cp); }
        }
        { extern unsigned long timer_ticks(void);   /* 100 Hz -> 10 ms/tick */
          extern void shell_flush_screen(void);      /* repaint mid-command      */
          unsigned long t0 = timer_ticks(), t1, t2, t3;
          uart_puts("wifi: bringing up firmware (please wait ~10 s)...\n");
          shell_flush_screen();                      /* show it before the block */
          if (wifi_probe() != 0) { uart_puts("wifi: bring-up FAILED — run wifi-invest\n"); shell_flush_screen(); return 1; }
          t1 = timer_ticks();
          uart_puts("wifi: firmware up ("); puts_dec((int)((t1-t0)*10)); uart_puts(" ms).\n");
          if (!ssid) {
              shell_flush_screen();
              int n = wifi_scan_run();
              uart_puts("wifi: radio up, "); puts_dec(n);
              uart_puts(" APs found. Connect with: wifi on <ssid> <pass>\n");
              shell_flush_screen();
              return 0;
          }
          uart_puts("wifi: joining \""); uart_puts(ssid); uart_puts("\" (WPA2)...\n");
          shell_flush_screen();
          if (wifi_join_run(ssid, pass) != 0) {
              uart_puts("wifi: JOIN FAILED — check ssid/pass, or run wifi-invest\n");
              shell_flush_screen(); return 1;
          }
          t2 = timer_ticks();
          uart_puts("wifi: associated ("); puts_dec((int)((t2-t1)*10)); uart_puts(" ms). DHCP...\n");
          shell_flush_screen();
          if (wifi_dhcp() != 0) {
              uart_puts("wifi: DHCP FAILED — run wifi-invest\n");
              shell_flush_screen(); return 1;
          }
          t3 = timer_ticks();
          { unsigned char ip[4]; wifi_ipaddr(ip);
            uart_puts("wifi: CONNECTED  IP="); puts_ip(ip);
            uart_puts("  (dhcp "); puts_dec((int)((t3-t2)*10));
            uart_puts(" ms, total "); puts_dec((int)((t3-t0)*10)); uart_puts(" ms)\n"); }
          shell_flush_screen();
        }
        return 0;
    }
    if (str_eq(argv[1], "adhoc")) {      /* wifi adhoc <ssid> [ch] [node] — MANET ad-hoc (IBSS), ip 10.0.0.n */
        extern int wifi_adhoc(const char *ssid, int channel, int n);
        extern void shell_flush_screen(void);
        int ch = 6, node = 1; const char *p;
        if (argc < 3) { uart_puts("usage: wifi adhoc <ssid> [ch] [node]\n"); return 1; }
        if (argc >= 4) { ch = 0;   for (p = argv[3]; *p>='0'&&*p<='9'; p++) ch   = ch*10   + (*p-'0'); }
        if (argc >= 5) { node = 0; for (p = argv[4]; *p>='0'&&*p<='9'; p++) node = node*10 + (*p-'0'); }
        uart_puts("wifi: joining ad-hoc cell (IBSS) — please wait...\n");
        shell_flush_screen();
        if (wifi_adhoc(argv[2], ch, node) != 0) { uart_puts("wifi: adhoc FAILED\n"); shell_flush_screen(); return 1; }
        uart_puts("wifi: ad-hoc up, ip 10.0.0."); puts_dec(node);
        uart_puts(" (AODV relay active)\n");
        shell_flush_screen();
        return 0;
    }
    if (str_eq(argv[1], "aodv")) {       /* wifi aodv <a.b.c.d> — on-demand multi-hop route discovery */
        extern int wifi_aodv(const unsigned char *dst);
        extern void shell_flush_screen(void);
        unsigned char ip[4] = {0,0,0,0}; int oct = 0, val = 0; const char *p;
        if (argc < 3) { uart_puts("usage: wifi aodv <a.b.c.d>\n"); return 1; }
        for (p = argv[2]; ; p++) {
            if (*p >= '0' && *p <= '9') val = val*10 + (*p - '0');
            else { if (oct < 4) ip[oct] = (unsigned char)val; oct++; val = 0; if (!*p) break; }
        }
        uart_puts("wifi: AODV route discovery — please wait...\n");
        shell_flush_screen();
        wifi_aodv(ip);
        shell_flush_screen();
        return 0;
    }
    uart_puts("wifi: unknown subcommand — use on/off/status/scan/adhoc/aodv\n");
    return 1;
}

/* wifi-invest: maintenance / diagnostics for when the connection won't come up.
 * Re-runs the full M0..M1 bring-up, dumps the pinmux + the detailed trace log,
 * then scans, so the failing stage is visible. */
static int cmd_wifi_invest(int argc, char **argv)
{
    int rc, n;
    (void)argc; (void)argv;
    uart_puts("=== wifi-invest: diagnostics ===\n");
    uart_puts("status: ");
    if (wifi_connected()) {
        unsigned char ip[4]; wifi_ipaddr(ip);
        uart_puts("connected ssid=\""); uart_puts(wifi_ssid());
        uart_puts("\" ip="); puts_ip(ip); uart_putc('\n');
    } else uart_puts("not connected\n");

    uart_puts("-- re-running full bring-up (M0..M1) --\n");
    rc = wifi_probe();
    uart_puts("bring-up rc="); puts_dec(rc);
    uart_puts(rc == 0 ? " (firmware up)\n" : " (FAILED — see trace below)\n");
    if (rc == 0) {
        n = wifi_scan_run();
        uart_puts("scan: "); puts_dec(n); uart_puts(" APs\n");
    }
    uart_puts("-- trace log --\n");
    uart_puts(wifi_trace());
    uart_putc('\n');
    uart_puts("=== end wifi-invest ===\n");
    return 0;
}

static int cmd_sdtest(int argc, char **argv)
{
    extern void sd_diag(void);
    (void)argc; (void)argv;
    sd_diag();
    return 0;
}

static const struct centry commandtab[] = {
    { "help",   "list the commands",                       cmd_help   },
    { "echo",   "echo the remaining words back",           cmd_echo   },
    { "wine",   "spin a 3D wireframe wine glass (Graphics)", cmd_wine  },
    { "4lines", "spin 4 segments on a square's corners",   cmd_4lines },
    { "kodama", "spin 3D block text \"KODAMA\" (Graphics)", cmd_kodama },
    { "clear",  "clear the shell window",                  cmd_clear  },
    { "pwd",    "print working directory",                 cmd_pwd    },
    { "cd",     "cd <dir>  change directory",              cmd_cd     },
    { "ls",     "ls [path]  list directory",               cmd_ls     },
    { "cat",    "cat <file>  print file contents",         cmd_cat    },
    { "kexec",  "kexec /microsd/<k.img>  boot another kernel from microSD", cmd_kexec },
    { "cc",     "cc <file>  compile + run a C/AIPL program (own process)", cmd_cc },
    { "make",   "make [file]  build Makefile + executable-form file, then run", cmd_make },
    { "run",    "run <file>   execute an executable-form file built by make",   cmd_run  },
    { "edit",   "edit <file>  emacs-style full-screen editor (serial terminal)", cmd_edit },
    { "abcl2c", "abcl2c <f.abcl>  translate real AIPL to C (writes <f>.c)",      cmd_abcl2c },
    { "hello",  "smoke marker — say hello",                cmd_hello  },
    { "mem",    "show __bss_start / __bss_end / _end",     cmd_mem    },
    { "peek",   "peek <hex_addr> — read 32-bit MMIO word", cmd_peek   },
    { "uptime", "raw CNTPCT_EL0 (generic timer)",          cmd_uptime },
    { "ps",       "core / EL status (no scheduler yet)",   cmd_ps       },
    { "halt",     "PSCI SYSTEM_OFF + WFE park",            cmd_halt     },
    { "pingpong", "2-actor cooperative PingPong [rounds]", cmd_pingpong },
    { "procdemo", "real 2-process ctxsw demo [iters]",     cmd_procdemo },
    { "usb",      "DWC2 USB HCD diagnostics (Pi 4 only)",  cmd_usb      },
    { "ticks",    "show 100 Hz timer tick counter",        cmd_ticks    },
    { "rxstat",   "drain RX ring + show pkt/byte counters", cmd_rxstat  },
    { "tcpstat",  "show TCP listener state + handshake ctrs", cmd_tcpstat },
    { "pan",      "pan <dx> <dy>  scroll viewport",        cmd_pan      },
    { "view",     "show viewport / desktop sizes",         cmd_view     },
    { "autopan",  "autopan [on|off]  toggle demo scroll",  cmd_autopan  },
    { "reboot",   "reboot the board (BCM2712 PM watchdog)",  cmd_reboot   },
    { "wifi",       "wifi on <ssid> <pass> | off | status | scan | adhoc <ssid> [ch] [node] | aodv <ip>", cmd_wifi },
    { "wifi-invest","wifi diagnostics + maintenance (re-run bring-up, dump trace)", cmd_wifi_invest },
    { "sdtest",     "SD card controller diagnostics (read LBA 0)", cmd_sdtest },
    { "?",      "alias for help",                          cmd_help   },
    { 0, 0, 0 }
};

/* ---------- main REPL ---------- */

static int tokenise(char *line, char **tok)
{
    int n = 0;
    char *p = line;
    while (*p && n < SHELL_MAXTOK) {
        /* skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        tok[n++] = p;
        /* skip word */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

int shell_dispatch_line(char *line)
{
    static char *tok[SHELL_MAXTOK];
    int ntok = tokenise(line, tok);
    if (ntok == 0) return 0;

    const struct centry *e;
    for (e = commandtab; e->name; e++) {
        if (str_eq(tok[0], e->name)) {
            e->fn(ntok, tok);
            return 0;
        }
    }
    uart_puts("?command-not-found: ");
    uart_puts(tok[0]);
    uart_puts(" (try `help`)\n");
    return 0;
}

void shell_main(void)
{
    static char line[SHELL_BUFLEN];

    uart_puts("type `help` for the command list.\n");

    for (;;) {
        uart_puts("xinu-pi5$ ");
        int n = uart_getline(line, SHELL_BUFLEN);
        if (n <= 0) continue;
        shell_dispatch_line(line);
    }
}
