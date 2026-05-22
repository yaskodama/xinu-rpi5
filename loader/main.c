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

/* Static window descriptors — laid out after video_init() picks the
 * actual screen dimensions in kernel_main(). */
static window_t banner_win;
static window_t status_win;
static window_t anim_win;

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

    uart_puts("\n");
    uart_puts("Round 1: B/U/M1/S0/X0 done.\n");

    /* If the HDMI framebuffer came up, hand off to the window
     * system (auto-driven demos: banner + system status + bouncing
     * box).  If not, fall back to the UART-only REPL so a serial
     * console can still drive Xinu. */
    if (video_rc == 0) {
        uart_puts("Entering window system (wm_run, no input)...\n");

        unsigned int sw = video_screen_width();
        /* unsigned int sh = video_screen_height(); -- not needed */

        /* Title-bar window: full width, 56 px tall, top of screen. */
        banner_win.x = 0;
        banner_win.y = 0;
        banner_win.width  = (int)sw;
        banner_win.height = 56;
        const char *bt = "Xinu " BOARD_NAME " on " SOC_NAME;
        for (int i = 0; i < WM_TITLE_MAX && bt[i]; i++) banner_win.title[i] = bt[i];
        banner_win.chrome_color = 0xFFAACCEEU;
        banner_win.title_bg     = 0xFF0040A0U;
        banner_win.title_fg     = 0xFFFFFFFFU;
        banner_win.content_bg   = 0xFF101820U;
        banner_win.draw_content = win_banner;
        wm_add(&banner_win);

        /* Status window: left half, below the title bar. */
        status_win.x = 0;
        status_win.y = 60;
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

        /* Animation window: right of status, same vertical band. */
        anim_win.x = 324;
        anim_win.y = 60;
        anim_win.width  = (int)sw - 324;
        anim_win.height = 200;
        const char *an = "Animation";
        for (int i = 0; i < WM_TITLE_MAX && an[i]; i++) anim_win.title[i] = an[i];
        anim_win.chrome_color = 0xFFFFAA60U;
        anim_win.title_bg     = 0xFF704020U;
        anim_win.title_fg     = 0xFFFFFFFFU;
        anim_win.content_bg   = 0xFF000000U;
        anim_win.draw_content = win_anim;
        wm_add(&anim_win);

        wm_run();   /* never returns */
    }

    uart_puts("Entering interactive shell (no HDMI)...\n");

    /* Hand off to the bare-metal REPL (never returns). */
    shell_main();
}
