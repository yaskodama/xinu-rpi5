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
//   reboot               watchdog-driven reset via RP1 (stub — needs
//                        more work; for now just spins)
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
    static char src[4096];
    int len = vfs_read(n, src, sizeof src - 1);
    src[len] = 0;

    uart_puts("cc: compiling + running "); uart_puts(rp);
    uart_puts(" (separate process)\n");

    /* Compile AND run in a dedicated process on a STATIC stack, so a buggy
     * program (deep recursion, runaway loop, bad pointer) can only blow that
     * stack and trip the runaway-loop deadline — never freeze the board.  A
     * static stack (not getmem) is mandatory: cc/make are dispatched from
     * genet_rx_tick (USB-keyboard pump + HTTP /run), where getmem() — which is
     * not reentrant against the main thread — would race and wedge the box. */
    static unsigned char ccrun_stack[CCRUN_STK] __attribute__((aligned(16)));
    g_ccrun_src = src; g_ccrun_len = len;
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

static int cmd_make(int argc, char **argv)
{
    /* `make` is a convenience alias for `cc`; default target is the C sample.
     * Both compile + run locally via a dedicated process (not the inline JIT). */
    return cc_compile_and_run(argc >= 2 ? argv[1] : "/home/hello.c");
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
    (void)argc; (void)argv;
    uart_puts("reboot: RP1 watchdog not wired up yet — spinning in WFE.\n");
    uart_puts("        (power-cycle the board to recover)\n");
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
                uart_puts("usage: wifi on <ssid> <pass> | wifi on | wifi off | wifi scan | wifi status\n");
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
        const char *ssid = 0, *pass = 0;
        if (argc >= 4) {                 /* wifi on <ssid> <pass>: remember + use */
            ssid = argv[2]; pass = argv[3];
            str_copy(g_wifi_ssid, ssid, sizeof g_wifi_ssid);
            str_copy(g_wifi_pass, pass, sizeof g_wifi_pass);
            g_wifi_have_creds = 1;
        } else if (g_wifi_have_creds) {  /* wifi on: reuse last creds (RAM only) */
            ssid = g_wifi_ssid; pass = g_wifi_pass;
        }
        uart_puts("wifi: bringing up firmware (takes a few seconds)...\n");
        if (wifi_probe() != 0) { uart_puts("wifi: bring-up FAILED — run wifi-invest\n"); return 1; }
        uart_puts("wifi: firmware up.\n");
        if (!ssid) {
            int n = wifi_scan_run();
            uart_puts("wifi: radio up, "); puts_dec(n);
            uart_puts(" APs found. Connect with: wifi on <ssid> <pass>\n");
            return 0;
        }
        uart_puts("wifi: joining \""); uart_puts(ssid); uart_puts("\" (WPA2)...\n");
        if (wifi_join_run(ssid, pass) != 0) {
            uart_puts("wifi: JOIN FAILED — check ssid/pass, or run wifi-invest\n");
            return 1;
        }
        uart_puts("wifi: associated. requesting DHCP...\n");
        if (wifi_dhcp() != 0) {
            uart_puts("wifi: DHCP FAILED — run wifi-invest\n");
            return 1;
        }
        { unsigned char ip[4]; wifi_ipaddr(ip);
          uart_puts("wifi: CONNECTED.  IP="); puts_ip(ip); uart_putc('\n'); }
        return 0;
    }
    uart_puts("wifi: unknown subcommand — use on/off/status/scan\n");
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
    { "cc",     "cc <file>  compile + run a C/AIPL program (own process)", cmd_cc },
    { "make",   "make [file]  compile + run a program (alias for cc)", cmd_make  },
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
    { "reboot",   "stub — spins until power-cycle",        cmd_reboot   },
    { "wifi",       "wifi on <ssid> <pass> | off | status | scan", cmd_wifi },
    { "wifi-invest","wifi diagnostics + maintenance (re-run bring-up, dump trace)", cmd_wifi_invest },
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
