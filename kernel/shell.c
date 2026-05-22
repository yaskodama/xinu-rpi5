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

/* Linker-script symbols (from kernel/link.ld). */
extern unsigned long __bss_start;
extern unsigned long __bss_end;
extern unsigned long _end;

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

static const struct centry commandtab[] = {
    { "help",   "list the commands",                       cmd_help   },
    { "echo",   "echo the remaining words back",           cmd_echo   },
    { "hello",  "smoke marker — say hello",                cmd_hello  },
    { "mem",    "show __bss_start / __bss_end / _end",     cmd_mem    },
    { "peek",   "peek <hex_addr> — read 32-bit MMIO word", cmd_peek   },
    { "uptime", "raw CNTPCT_EL0 (generic timer)",          cmd_uptime },
    { "ps",       "core / EL status (no scheduler yet)",   cmd_ps       },
    { "halt",     "PSCI SYSTEM_OFF + WFE park",            cmd_halt     },
    { "pingpong", "2-actor cooperative PingPong [rounds]", cmd_pingpong },
    { "procdemo", "real 2-process ctxsw demo [iters]",     cmd_procdemo },
    { "reboot",   "stub — spins until power-cycle",        cmd_reboot   },
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

void shell_main(void)
{
    static char line[SHELL_BUFLEN];
    static char *tok[SHELL_MAXTOK];

    uart_puts("type `help` for the command list.\n");

    for (;;) {
        uart_puts("xinu-pi5$ ");
        int n = uart_getline(line, SHELL_BUFLEN);
        if (n <= 0) continue;

        int ntok = tokenise(line, tok);
        if (ntok == 0) continue;

        const struct centry *e;
        for (e = commandtab; e->name; e++) {
            if (str_eq(tok[0], e->name)) {
                e->fn(ntok, tok);
                goto next;
            }
        }
        uart_puts("?command-not-found: ");
        uart_puts(tok[0]);
        uart_puts(" (try `help`)\n");
    next: ;
    }
}
