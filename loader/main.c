// kernel/main.c — Xinu-on-Pi-5 first sign of life.
//
// boot.S has cleared BSS, set up the initial stack and dropped here.
// All we do for the B0/B1/B2/U0/U1 milestone is bring up UART0 and
// print a hello banner so the USB-serial cable shows something the
// host can grep for ("Xinu Pi5 hello" is the smoke marker).
//
// Real Xinu init (interrupts, mmu, scheduler) lands in subsequent
// phases — those will pull in their own files (system/initialize.c,
// arch/aarch64/mmu.c, arch/aarch64/exception_vectors.S, …).  For now
// the kernel just hangs in a WFE loop after the banner so the human
// has time to read it before resetting the board.

#include "uart.h"
#include "shell.h"
#include "memory.h"
#include "proc.h"
#include "video.h"
#include "early_diag.h"
#include "wm.h"
#include "kmalloc.h"
#include "vfs.h"
#include "sd.h"
#include "fat32.h"
#include "usb.h"
#include "shellwin.h"
#include "softkbd.h"
#include "exception.h"
#include "gic.h"
#include "timer.h"
#include "irq.h"
#include "xhci.h"
#include "genet.h"

/* USPi is gone (DWC2 only — Pi 4 USB-A keyboards/mice need xHCI).
 * Keyboard input from the future xHCI HID driver will land here. */
void xhci_keyboard_event(char c)
{
    shellwin_handle_key(c);
}

static int  g_cursor_x = 320;
static int  g_cursor_y = 240;

void xhci_mouse_event(unsigned nButtons, int dx, int dy)
{
    g_cursor_x += dx;
    g_cursor_y += dy;
    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();
    wm_set_autopan(0);
    if (g_cursor_x < 0)        { wm_pan(g_cursor_x, 0);          g_cursor_x = 0; }
    if (g_cursor_y < 0)        { wm_pan(0, g_cursor_y);          g_cursor_y = 0; }
    if (g_cursor_x >= sw)      { wm_pan(g_cursor_x - sw + 1, 0); g_cursor_x = sw - 1; }
    if (g_cursor_y >= sh)      { wm_pan(0, g_cursor_y - sh + 1); g_cursor_y = sh - 1; }
    wm_cursor_set(g_cursor_x, g_cursor_y, 1);
    (void)nButtons;
}

extern unsigned char _end[];   /* set by link.ld — top of static image */

#ifndef HEAP_END
/* Pi 5 firmware: assume at least 1 GiB of RAM mapped starting at 0
 * (config.txt's `arm_64bit=1` gives us the whole low region).  QEMU
 * `virt` builds override this from the Makefile to 0x50000000 (256 MB). */
#define HEAP_END 0x40000000UL
#endif

static void puts_kb(unsigned long bytes)
{
    /* Tiny KB pretty-printer to avoid pulling printf in this early. */
    unsigned long kb = bytes >> 10;
    char buf[12];
    int n = 0;
    if (kb == 0) { uart_putc('0'); return; }
    while (kb > 0) { buf[n++] = (char)('0' + (kb % 10)); kb /= 10; }
    while (n--) uart_putc(buf[n]);
}

/* =====================================================================
 * Window-system demo content callbacks.
 *
 * These are passed to wm_add() and called by wm_run() once per frame.
 * They write into the content area of their owning window using the
 * draw_string_at / fill_rect primitives from video.c.
 * =====================================================================
 */

/* Render a non-negative unsigned long into `buf` as decimal.  Returns
 * pointer to the start of the produced digits (NUL terminated). */
static char *u_to_dec(unsigned long v, char *buf, int buflen)
{
    int n = buflen - 1;
    buf[n--] = 0;
    if (v == 0) { buf[n--] = '0'; }
    else { while (v && n >= 0) { buf[n--] = (char)('0' + (v % 10)); v /= 10; } }
    return &buf[n + 1];
}

static char *u_to_hex16(unsigned long v, char *buf)
{
    /* "0x" + 16 hex digits + NUL = 19 bytes; caller supplies ≥19. */
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        unsigned long nyb = (v >> ((15 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10));
    }
    buf[18] = 0;
    return buf;
}

static void win_banner(window_t *self, unsigned int frame)
{
    (void)frame;
    /* Static text inside the content area.  draw_string_at is at
     * pixel coordinates so we offset from the window origin. */
    draw_string_at(self->x + 8,  self->y + WM_TITLEBAR_H + 6,
        "Xinu " BOARD_NAME " Window System",
        0xFF00FF80U, self->content_bg);
    draw_string_at(self->x + 8,  self->y + WM_TITLEBAR_H + 18,
        "B U M1 S0 X0 -- cooperative scheduler",
        0xFFCCCCCCU, self->content_bg);
    draw_string_at(self->x + 8,  self->y + WM_TITLEBAR_H + 30,
        "no input -- HDMI-only demo (no USB stack)",
        0xFF888888U, self->content_bg);
}

static void win_status(window_t *self, unsigned int frame)
{
    char buf[64];
    char *p;
    unsigned long midr, mpidr, current_el, cnt, freq;
    int line = 0;
    int xb = self->x + 8;
    int yb = self->y + WM_TITLEBAR_H + 6;

    __asm__ volatile ("mrs %0, midr_el1"  : "=r"(midr));
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile ("mrs %0, currentel" : "=r"(current_el));
    __asm__ volatile ("mrs %0, cntpct_el0": "=r"(cnt));
    __asm__ volatile ("mrs %0, cntfrq_el0": "=r"(freq));

    unsigned int fg = 0xFFFFFFFFU;
    unsigned int bg = self->content_bg;

    draw_string_at(xb, yb + line*12, "System status", 0xFF80FF80U, bg); line++;

    /* MIDR */
    {
        char tmp[24];
        u_to_hex16(midr, tmp);
        draw_string_at(xb, yb + line*12, "MIDR_EL1:", fg, bg);
        draw_string_at(xb + 80, yb + line*12, tmp, fg, bg);
        line++;
    }

    /* Current EL */
    {
        char tmp[8] = {'E','L',' ',0};
        tmp[3] = (char)('0' + ((current_el >> 2) & 3));
        tmp[4] = 0;
        draw_string_at(xb, yb + line*12, "CurrentEL:", fg, bg);
        draw_string_at(xb + 80, yb + line*12, tmp, fg, bg);
        line++;
    }

    /* Core (MPIDR bits 0:7) */
    {
        char tmp[12];
        p = u_to_dec(mpidr & 0xFFUL, tmp, sizeof tmp);
        draw_string_at(xb, yb + line*12, "Core:", fg, bg);
        draw_string_at(xb + 80, yb + line*12, p, fg, bg);
        line++;
    }

    /* Uptime in seconds */
    {
        char tmp[24];
        unsigned long secs = freq ? (cnt / freq) : 0UL;
        p = u_to_dec(secs, tmp, sizeof tmp);
        draw_string_at(xb, yb + line*12, "Uptime:", fg, bg);
        draw_string_at(xb + 80, yb + line*12, p, fg, bg);
        draw_string_at(xb + 80 + 8 * (int)(tmp + sizeof tmp - 1 - p),
                       yb + line*12, " s", fg, bg);
        line++;
    }

    /* Heap free in KiB */
    {
        char tmp[24];
        p = u_to_dec(mem_free_bytes() >> 10, tmp, sizeof tmp);
        draw_string_at(xb, yb + line*12, "Heap free:", fg, bg);
        draw_string_at(xb + 80, yb + line*12, p, fg, bg);
        draw_string_at(xb + 80 + 8 * (int)(tmp + sizeof tmp - 1 - p),
                       yb + line*12, " KiB", fg, bg);
        line++;
    }

    /* Frame counter — proves we're alive even if uptime ticks slowly. */
    {
        char tmp[24];
        p = u_to_dec((unsigned long)frame, tmp, sizeof tmp);
        draw_string_at(xb, yb + line*12, "Frame:", fg, bg);
        draw_string_at(xb + 80, yb + line*12, p, fg, bg);
        line++;
    }
}

static void win_anim(window_t *self, unsigned int frame)
{
    /* A 24x24 square that bounces back and forth in the content area.
     * Uses `frame` as time, no state stored anywhere. */
    int box = 24;
    int ax = self->x + 1;
    int ay = self->y + WM_TITLEBAR_H + 2;
    int aw = self->width  - 2 - box;
    int ah = self->height - WM_TITLEBAR_H - 3 - box;

    int period_x = aw * 2;
    int period_y = ah * 2;
    int px = (int)(frame * 3 % (unsigned)period_x);
    if (px > aw) px = period_x - px;
    int py = (int)(frame * 2 % (unsigned)period_y);
    if (py > ah) py = period_y - py;

    /* Pick a colour that drifts through hues over time. */
    unsigned int color;
    unsigned int phase = (frame >> 1) & 0xFF;
    if (phase < 0x55) {
        color = 0xFF000000U | (((0xFFu) << 16) | ((phase * 3) << 8) | 0);
    } else if (phase < 0xAA) {
        color = 0xFF000000U | (((0xAA - (phase - 0x55)) * 3 << 16) | (0xFF << 8) | 0);
    } else {
        color = 0xFF000000U | (0 | (0xFF << 8) | ((phase - 0xAA) * 3));
    }

    fill_rect(ax + px, ay + py, box, box, color);
}

/* ---- FAT32 → VFS bridge ----------------------------------------
 *
 * The visitor inserts each non-pseudo directory entry into the VFS
 * subtree we're building under /sd.  Directory recursion is one
 * level deep by default (depth budget) to keep boot time bounded —
 * a SD card with hundreds of overlays could easily produce 500+
 * entries otherwise.  Files do NOT have their contents copied yet;
 * the VFS node's size matches the on-disk size, but the data
 * buffer stays empty.  Reading contents on demand requires a
 * vfs_node_t open() hook which we can add later.
 */
#define SD_MAX_DEPTH        2     /* root + 1 level (0..2 inclusive)  */
#define SD_MAX_DIR_ENTRIES  256   /* per-directory entry cap          */

struct sd_walk_ctx {
    fat32_t      *fs;
    vfs_node_t   *parent[8];      /* parent VFS node per depth level  */
    int           remaining[8];   /* per-level entry budget           */
};

static void sd_visit(const char *name, int is_dir, unsigned long size,
                     unsigned int first_cluster, int depth, void *vctx)
{
    struct sd_walk_ctx *c = (struct sd_walk_ctx *)vctx;
    if (depth < 0 || depth >= 8)              return;
    if (c->parent[depth] == 0)                return;
    if (c->remaining[depth] <= 0)             return;
    c->remaining[depth]--;

    vfs_node_t *node;
    if (is_dir) {
        node = vfs_mkdir(c->parent[depth], name);
        if (node && depth + 1 < SD_MAX_DEPTH && depth + 1 < 8) {
            /* Recurse one cluster-chain deep.  Set up the next-level
             * context fields first so the recursive sd_visit calls
             * land in this newly-created VFS subdir. */
            c->parent[depth + 1]    = node;
            c->remaining[depth + 1] = SD_MAX_DIR_ENTRIES;
            fat32_walk_dir(c->fs, first_cluster, depth + 1, sd_visit, c);
            c->parent[depth + 1] = 0;
        }
    } else {
        node = vfs_create_file(c->parent[depth], name);
        if (node) {
            /* Record the real on-disk size without copying contents. */
            node->size = size;
        }
    }
}

/* Mount real SD-card FAT32 partition under /sd/.  No-op (graceful
 * fallback) if the controller fails to init or the partition isn't
 * FAT32; that lets the QEMU / Pi 5 builds compile and run identically
 * to before. */
static void vfs_mount_sd(void)
{
    vfs_node_t *sd = vfs_mkdir(vfs_root(), "sd");
    if (sd == 0) return;

    if (sd_init() != 0) return;

    fat32_t fs;
    if (fat32_mount(&fs) != 0) return;

    struct sd_walk_ctx ctx = {0};
    ctx.fs           = &fs;
    ctx.parent[0]    = sd;
    ctx.remaining[0] = SD_MAX_DIR_ENTRIES;
    fat32_walk_dir(&fs, fs.root_cluster, 0, sd_visit, &ctx);
}

/* ---- VFS demo: populate a small tree at boot ------------------- */
static void vfs_populate_demo(void)
{
    vfs_node_t *r = vfs_root();
    vfs_node_t *etc  = vfs_mkdir(r, "etc");
    vfs_node_t *proc = vfs_mkdir(r, "proc");
    vfs_node_t *home = vfs_mkdir(r, "home");

    if (etc) {
        vfs_node_t *f;
        f = vfs_create_file(etc,  "hostname"); vfs_write_str(f, "xinu-" BOARD_NAME);
        f = vfs_create_file(etc,  "version");  vfs_write_str(f, "Xinu Round 1 / " SOC_NAME);
        f = vfs_create_file(etc,  "kernel");   vfs_write_str(f, KERNEL_NAME);
    }
    if (proc) {
        char buf[24];
        unsigned long midr;
        __asm__ volatile ("mrs %0, midr_el1" : "=r"(midr));
        /* simple hex of MIDR for the file contents */
        buf[0] = '0'; buf[1] = 'x';
        for (int i = 0; i < 16; i++) {
            unsigned long nyb = (midr >> ((15 - i) * 4)) & 0xF;
            buf[2 + i] = (char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10));
        }
        buf[18] = 0;
        vfs_node_t *f = vfs_create_file(proc, "self.midr"); vfs_write_str(f, buf);
        f = vfs_create_file(proc, "self.el");    vfs_write_str(f, "EL1");
        vfs_mkdir(proc, "1");  /* placeholder dir for "process 1" */
        vfs_mkdir(proc, "2");
    }
    if (home) {
        vfs_node_t *user = vfs_mkdir(home, "user");
        if (user) {
            vfs_node_t *f;
            f = vfs_create_file(user, "readme.txt");
            vfs_write_str(f, "Welcome to the Xinu Window System on Pi 4.");
            f = vfs_create_file(user, "notes.txt");
            vfs_write_str(f, "kmalloc-backed tmpfs, no input yet.");
        }
    }
}

/* ---- File-tree window ------------------------------------------- */
struct ftree_ctx {
    int x, y;          /* cursor inside content area in PIXEL coords */
    int max_y;
    unsigned int fg;
    unsigned int bg;
    unsigned int dir_fg;
};

static int simple_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void ftree_visit_safe(int depth, vfs_node_t *node, void *vctx)
{
    struct ftree_ctx *c = (struct ftree_ctx *)vctx;
    if (c->y > c->max_y) return;
    int xb = c->x + depth * 16;
    if (depth > 0) draw_string_at(xb - 14, c->y, "|-", c->fg, c->bg);
    const char *nm = (depth == 0) ? "/" : node->name;
    unsigned int colour = (node->kind == VFS_DIR) ? c->dir_fg : c->fg;
    draw_string_at(xb, c->y, nm, colour, c->bg);
    if (node->kind == VFS_DIR) {
        draw_string_at(xb + 8 * simple_strlen(nm), c->y, "/", colour, c->bg);
    }
    c->y += 10;
}

static void win_ftree(window_t *self, unsigned int frame)
{
    (void)frame;
    /* Clear content (chrome was just redrawn so content_bg is fresh) */
    struct ftree_ctx ctx = {
        .x      = self->x + 8,
        .y     = self->y + WM_TITLEBAR_H + 6,
        .max_y = self->y + self->height - 12,
        .fg    = 0xFFCCCCCCU,
        .bg    = self->content_bg,
        .dir_fg= 0xFF60D0FFU
    };
    vfs_walk(vfs_root(), 0, ftree_visit_safe, &ctx);
}

/* ---- Memory window ---------------------------------------------- */
static void win_mem(window_t *self, unsigned int frame)
{
    (void)frame;
    char tmp[24];
    unsigned int fg = 0xFFFFFFFFU;
    unsigned int hi = 0xFFFFD060U;
    unsigned int bg = self->content_bg;
    int xb = self->x + 8;
    int yb = self->y + WM_TITLEBAR_H + 6;
    int line = 0;

    draw_string_at(xb, yb + (line++) * 10, "Heap (getmem)",   hi, bg);
    char *p;
    p = u_to_dec(mem_total_bytes() >> 10, tmp, sizeof tmp);
    draw_string_at(xb, yb + line * 10, "  total:", fg, bg);
    draw_string_at(xb + 88, yb + (line++) * 10, p, fg, bg);

    p = u_to_dec(mem_free_bytes() >> 10, tmp, sizeof tmp);
    draw_string_at(xb, yb + line * 10, "  free :", fg, bg);
    draw_string_at(xb + 88, yb + (line++) * 10, p, fg, bg);

    p = u_to_dec(mem_largest_block() >> 10, tmp, sizeof tmp);
    draw_string_at(xb, yb + line * 10, "  larg :", fg, bg);
    draw_string_at(xb + 88, yb + (line++) * 10, p, fg, bg);

    line++;
    draw_string_at(xb, yb + (line++) * 10, "kmalloc",         hi, bg);
    p = u_to_dec(kmalloc_live_blocks(), tmp, sizeof tmp);
    draw_string_at(xb, yb + line * 10, "  live :", fg, bg);
    draw_string_at(xb + 88, yb + (line++) * 10, p, fg, bg);

    p = u_to_dec(kmalloc_live_bytes(), tmp, sizeof tmp);
    draw_string_at(xb, yb + line * 10, "  bytes:", fg, bg);
    draw_string_at(xb + 88, yb + (line++) * 10, p, fg, bg);

    p = u_to_dec(kmalloc_total_allocs(), tmp, sizeof tmp);
    draw_string_at(xb, yb + line * 10, "  allocs:", fg, bg);
    draw_string_at(xb + 88, yb + (line++) * 10, p, fg, bg);

    p = u_to_dec(kmalloc_total_frees(), tmp, sizeof tmp);
    draw_string_at(xb, yb + line * 10, "  frees:", fg, bg);
    draw_string_at(xb + 88, yb + (line++) * 10, p, fg, bg);

    line++;
    draw_string_at(xb, yb + (line++) * 10, "VFS",              hi, bg);
    p = u_to_dec(vfs_node_count(), tmp, sizeof tmp);
    draw_string_at(xb, yb + line * 10, "  nodes:", fg, bg);
    draw_string_at(xb + 88, yb + (line++) * 10, p, fg, bg);

    p = u_to_dec(vfs_total_file_bytes(), tmp, sizeof tmp);
    draw_string_at(xb, yb + line * 10, "  bytes:", fg, bg);
    draw_string_at(xb + 88, yb + (line++) * 10, p, fg, bg);
}

/* Static window descriptors — laid out after video_init() picks the
 * actual screen dimensions in kernel_main(). */
static window_t banner_win;
static window_t status_win;
static window_t anim_win;
static window_t ftree_win;
static window_t mem_win;

void kernel_main(void)
{
    unsigned long heap_start;
    int video_rc;

    /* ---------- AGGRESSIVE LIVENESS DIAGNOSTIC --------------------
     * Paint distinctive 256x256 colour blocks at six candidate
     * framebuffer addresses BEFORE anything else.  If the kernel
     * is running at all, at least one of those blocks will land
     * on (or near) the firmware's framebuffer and visibly stamp
     * over the rainbow boot pattern — telling us simultaneously
     * "the kernel runs" and "the framebuffer lives around HERE".
     * Safe on Pi 5 because all candidate addresses are within
     * mapped low-4-GiB RAM; QEMU virt only has 256 MiB so the
     * higher candidates fault, but with -DSKIP_MBOX we don't run
     * this in QEMU at all. */
    early_paint_diagnostic();

    uart_init();
    shellwin_init();   /* must run before the first uart_putc so the
                          ring captures the boot banner */

    /* S1a — install VBAR_EL1 so a stray data abort surfaces on the
     *       UART/shell window instead of jumping to address 0 and
     *       silently freezing the kernel. */
    exception_init();

    /* Try to bring up the HDMI framebuffer before printing anything,
     * so the banner appears on both UART and the monitor.  Failure
     * is benign — on QEMU virt and on Pi 5 revisions where the VC
     * mailbox is elsewhere this just leaves screen_putc() as a no-op. */
    video_rc = video_init();

    /* BOARD_NAME / SOC_NAME / KERNEL_NAME come from the Makefile
     * (-DBOARD_NAME=\"Pi4\" etc.) so each variant prints a banner
     * that matches the hardware it was built for. */
#ifndef BOARD_NAME
#define BOARD_NAME  "?"
#endif
#ifndef SOC_NAME
#define SOC_NAME    "?"
#endif
#ifndef KERNEL_NAME
#define KERNEL_NAME "?"
#endif
#define _STR(s) #s
#define STR(s)  _STR(s)

    uart_puts("\n");
    uart_puts("================================================\n");
    uart_puts("  Xinu " BOARD_NAME " hello (AArch64, " SOC_NAME ", " KERNEL_NAME ")\n");
    uart_puts("  PL011 UART0 @ " STR(UART0_BASE) ", 115200 8N1\n");
    uart_puts("  bootstrap: leex-style stub + xinu-rpi main\n");
    uart_puts("================================================\n");
    uart_puts("\n");
    if (video_rc == 0) {
        uart_puts("video: HDMI framebuffer up (640x480x32, 8x8 font)\n");
    } else {
        uart_puts("video: no HDMI framebuffer (mailbox timed out)\n");
    }

    /* M1 — bring up the first-fit kernel heap between _end and
     *      HEAP_END.  Everything after this point (proc stacks,
     *      shell command-time allocations) goes through getmem(). */
    heap_start = (unsigned long)_end;
    mem_init(heap_start, HEAP_END);
    uart_puts("heap: ");
    puts_kb(mem_total_bytes());
    uart_puts(" KiB available\n");

    /* S0 — initialise the process table.  Slot 0 (NULLPROC) inherits
     *      the current execution context: it is the boot/shell thread,
     *      runs whenever the ready list is empty, never sits on it. */
    proc_init();
    uart_puts("sched: cooperative ctxsw ready (NULLPROC = shell)\n");

    /* M1.5 — populate a small in-memory hierarchical filesystem so
     *        the file-tree window has something interesting to show
     *        and the kmalloc/VFS counters in the memory window are
     *        non-zero at boot. */
    vfs_populate_demo();
    {
        unsigned long count = vfs_node_count();
        uart_puts("vfs: tmpfs populated (");
        puts_kb(count << 10);
        uart_puts(" nodes, ");
        puts_kb(vfs_total_file_bytes() << 10);
        uart_puts(" file bytes)\n");
    }

    /* Mount the SD card's FAT32 partition under /sd/.  Silently
     * skipped on builds where SD_BASE isn't defined (Pi 5 / QEMU) —
     * the /sd directory will simply not appear in the VFS tree. */
    vfs_mount_sd();
    {
        unsigned long count = vfs_node_count();
        uart_puts("sd: total VFS now ");
        puts_kb(count << 10);
        uart_puts(" nodes\n");
    }

    /* USB-M0 — power on the DWC2 HCD via VC mailbox and read
     *           GSNPSID to confirm the controller answers MMIO.
     *           Skips MMIO probe if the mailbox call fails so a
     *           data abort can't brick the desktop. */
    usb_init();

    /* S1b/c/d — GIC + generic timer + IRQ unmask.  After this
     * point a 100 Hz IRQ runs in the background; timer_ticks()
     * advances and any future USPi handler attached via
     * connect_interrupt() will fire. */
    gic_init();
    timer_init();
    uart_puts("gic+timer: 100 Hz PPI 30 armed; unmasking DAIF.I\n");
    irq_enable_all();

    /* XHCI-A — PCIe-1 controller MMIO probe.  Skipped at boot: the
     * controller is clock/power-gated until we implement CPRMAN
     * + brcmstb-pcie bring-up, so the dump just slows the log.
     * Re-enable once XHCI-B lands. */
#if 0
    xhci_init();
#endif

    /* NET-A — probe BCM2711 GENET Ethernet MAC.  Goal today: see
     * if SYS_REV_CTRL responds with a sane value (expected to be
     * ~0x06000000 on Pi 4) so we know the controller is powered. */
    genet_init();

    uart_puts("\n");
    uart_puts("Round 1: B/U/M1/S0/X0 done.\n");

    /* If the HDMI framebuffer came up, hand off to the window
     * system (auto-driven demos: banner + system status + bouncing
     * box).  If not, fall back to the UART-only REPL so a serial
     * console can still drive Xinu. */
    if (video_rc == 0) {
        uart_puts("Entering window system (wm_run, no input)...\n");

        unsigned int sw = video_screen_width();
        unsigned int sh = video_screen_height();

        /* 5-window layout (640x480):
         *   +--- title 640x40 -----------------+
         *   | status 320x200 | animation 316x200 |
         *   | ftree  320x200 | memory   316x200 |
         */

        /* Title-bar window: full *virtual* desktop width, slimmer
         * to make room for everything below.  Only the part inside
         * the viewport (0..sw) shows at any moment. */
        banner_win.x = 0;
        banner_win.y = 0;
        banner_win.width  = WM_DESKTOP_W;
        banner_win.height = 28;
        const char *bt = "Xinu " BOARD_NAME " on " SOC_NAME;
        for (int i = 0; i < WM_TITLE_MAX && bt[i]; i++) banner_win.title[i] = bt[i];
        banner_win.chrome_color = 0xFFAACCEEU;
        banner_win.title_bg     = 0xFF0040A0U;
        banner_win.title_fg     = 0xFFFFFFFFU;
        banner_win.content_bg   = 0xFF101820U;
        banner_win.draw_content = win_banner;
        wm_add(&banner_win);

        /* Status window: right side of the initial viewport so
         * the shell on the left has room to show the full log. */
        status_win.x = 320;
        status_win.y = 32;
        status_win.width  = 320;
        status_win.height = 200;
        const char *st = "System status";
        for (int i = 0; i < WM_TITLE_MAX && st[i]; i++) status_win.title[i] = st[i];
        status_win.chrome_color = 0xFF60FF60U;
        status_win.title_bg     = 0xFF205020U;
        status_win.title_fg     = 0xFFFFFFFFU;
        status_win.content_bg   = 0xFF101810U;
        status_win.draw_content = win_status;
        wm_add(&status_win);

        /* File-tree window: well off the initial viewport so the
         * user can pan right to discover it. */
        ftree_win.x = WM_DESKTOP_W - 320;
        ftree_win.y = 32;
        ftree_win.width  = 320;
        ftree_win.height = 200;
        const char *ft = "VFS tree";
        for (int i = 0; i < WM_TITLE_MAX && ft[i]; i++) ftree_win.title[i] = ft[i];
        ftree_win.chrome_color = 0xFF60D0FFU;
        ftree_win.title_bg     = 0xFF205070U;
        ftree_win.title_fg     = 0xFFFFFFFFU;
        ftree_win.content_bg   = 0xFF101418U;
        ftree_win.draw_content = win_ftree;
        wm_add(&ftree_win);

        /* Memory window: right side, below status so it sits in
         * the initial viewport with the shell on the left. */
        mem_win.x = 320;
        mem_win.y = 240;
        mem_win.width  = 320;
        mem_win.height = 220;
        const char *mt = "Memory";
        for (int i = 0; i < WM_TITLE_MAX && mt[i]; i++) mem_win.title[i] = mt[i];
        mem_win.chrome_color = 0xFFFFE060U;
        mem_win.title_bg     = 0xFF604020U;
        mem_win.title_fg     = 0xFFFFFFFFU;
        mem_win.content_bg   = 0xFF181410U;
        mem_win.draw_content = win_mem;
        wm_add(&mem_win);

        /* Shell window: left half of the *initial* viewport — the
         * boot log lives here and must stay visible without any
         * scrolling so the user can read what happened during
         * USPi init. */
        shell_win.x = 0;
        shell_win.y = 32;
        shell_win.width  = 320;
        shell_win.height = 432;
        const char *swt = "Shell (UART)";
        for (int i = 0; i < WM_TITLE_MAX && swt[i]; i++) shell_win.title[i] = swt[i];
        shell_win.chrome_color = 0xFF80E080U;
        shell_win.title_bg     = 0xFF205020U;
        shell_win.title_fg     = 0xFFFFFFFFU;
        shell_win.content_bg   = 0xFF000010U;
        shell_win.draw_content = shellwin_draw;
        wm_add(&shell_win);
        wm_set_tick(shellwin_step);

        /* Soft keyboard window: bottom-left of the initial 640×480
         * viewport.  Half-size as the user requested. */
        softkbd_win.x = 0;
        softkbd_win.y = 360;
        softkbd_win.width  = 320;
        softkbd_win.height = 120;
        const char *kbt = "Soft keyboard";
        for (int i = 0; i < WM_TITLE_MAX && kbt[i]; i++) softkbd_win.title[i] = kbt[i];
        softkbd_win.chrome_color = 0xFFFFB060U;
        softkbd_win.title_bg     = 0xFF704020U;
        softkbd_win.title_fg     = 0xFFFFFFFFU;
        softkbd_win.content_bg   = 0xFF0A0A14U;
        softkbd_win.draw_content = softkbd_draw;
        wm_add(&softkbd_win);

        /* Start the cursor at the centre of the *screen* (not the
         * virtual desktop) so it stays in view as the viewport
         * pans.  USPi (later) will drive it from mouse reports. */
        wm_cursor_set((int)sw / 2, (int)sh / 2, 1);

        wm_run();   /* never returns */
    }

    uart_puts("Entering interactive shell (no HDMI)...\n");

    /* Hand off to the bare-metal REPL (never returns). */
    shell_main();
}
