// device/video/video.c — HDMI framebuffer + 8x8 text console.

#include "video.h"
#include "mbox.h"

/* 16-byte aligned request buffer for the framebuffer-allocation
 * property tag sequence.  The exact tag IDs and layout are the
 * standard VC mailbox property interface, unchanged from Pi 2/3/4. */
static volatile unsigned int __attribute__((aligned(16))) mbox_buf[36];

static volatile unsigned char *fb_base;
static unsigned int            fb_pitch;
static unsigned int            fb_width;
static unsigned int            fb_height;
static int                     fb_ready;

static int cursor_col;
static int cursor_row;
#define COLS  (SCREEN_WIDTH  / FONT_WIDTH)
#define ROWS  (SCREEN_HEIGHT / FONT_HEIGHT)

/* ARGB8888 packing: B in byte 0 ... A in byte 3, which is what the
 * VC firmware hands us when we ask for depth=32 + BGR pixel order. */
static unsigned int color_fg = 0x00FFFFFFu;  /* white */
static unsigned int color_bg = 0x00101820u;  /* near-black */

int video_init(void)
{
    /* Build the property-tag request: set physical & virtual
     * dimensions, set depth, set pixel order, allocate buffer,
     * fetch the pitch. */
    int i = 0;
    mbox_buf[i++] = 0;             /* total size in bytes (patched below) */
    mbox_buf[i++] = 0;             /* request                              */

    mbox_buf[i++] = 0x48003;       /* tag: set physical (display) size     */
    mbox_buf[i++] = 8; mbox_buf[i++] = 8;
    mbox_buf[i++] = SCREEN_WIDTH; mbox_buf[i++] = SCREEN_HEIGHT;

    mbox_buf[i++] = 0x48004;       /* tag: set virtual (buffer) size       */
    mbox_buf[i++] = 8; mbox_buf[i++] = 8;
    mbox_buf[i++] = SCREEN_WIDTH; mbox_buf[i++] = SCREEN_HEIGHT;

    mbox_buf[i++] = 0x48009;       /* tag: set virtual offset              */
    mbox_buf[i++] = 8; mbox_buf[i++] = 8;
    mbox_buf[i++] = 0; mbox_buf[i++] = 0;

    mbox_buf[i++] = 0x48005;       /* tag: set depth                       */
    mbox_buf[i++] = 4; mbox_buf[i++] = 4;
    mbox_buf[i++] = SCREEN_DEPTH;

    mbox_buf[i++] = 0x48006;       /* tag: set pixel order                 */
    mbox_buf[i++] = 4; mbox_buf[i++] = 4;
    mbox_buf[i++] = 1;             /* 1 = RGB                              */

    mbox_buf[i++] = 0x40001;       /* tag: allocate buffer                 */
    mbox_buf[i++] = 8; mbox_buf[i++] = 8;
    mbox_buf[i++] = 4096; mbox_buf[i++] = 0;   /* alignment hint / out: addr+size */

    mbox_buf[i++] = 0x40008;       /* tag: get pitch                       */
    mbox_buf[i++] = 4; mbox_buf[i++] = 4;
    mbox_buf[i++] = 0;

    mbox_buf[i++] = 0;             /* end tag                              */

    mbox_buf[0] = i * 4;           /* total size                           */

    if (mbox_call(mbox_buf) < 0) return -1;

    /* Response patches the allocate-buffer slot with the base addr
     * (low 32 bits — high bits go in the size slot for very large
     * pages; in practice Pi gives us a low-mem address).  Bus address
     * needs the high bit (0x40000000) cleared on Pi 2/3/4/5. */
    unsigned int alloc_idx = 0;
    {
        unsigned int j = 2;
        while (j < (unsigned)i) {
            if (mbox_buf[j] == 0x40001) { alloc_idx = j + 3; break; }
            j += 3 + (mbox_buf[j+1] >> 2);
        }
    }
    if (alloc_idx == 0) return -1;

    unsigned int fb_addr = mbox_buf[alloc_idx] & 0x3FFFFFFFu;

    /* Pitch index: rescan for 0x40008 tag's value slot. */
    unsigned int pitch_idx = 0;
    {
        unsigned int j = 2;
        while (j < (unsigned)i) {
            if (mbox_buf[j] == 0x40008) { pitch_idx = j + 3; break; }
            j += 3 + (mbox_buf[j+1] >> 2);
        }
    }
    if (pitch_idx == 0) return -1;

    fb_base   = (volatile unsigned char *)(unsigned long)fb_addr;
    fb_pitch  = mbox_buf[pitch_idx];
    fb_width  = SCREEN_WIDTH;
    fb_height = SCREEN_HEIGHT;
    fb_ready  = 1;

    /* Clear screen to background colour. */
    for (unsigned int y = 0; y < fb_height; y++) {
        unsigned int *row = (unsigned int *)(fb_base + y * fb_pitch);
        for (unsigned int x = 0; x < fb_width; x++) row[x] = color_bg;
    }
    cursor_col = 0;
    cursor_row = 0;
    return 0;
}

int screen_ready(void) { return fb_ready; }

static void draw_glyph(int col, int row, char c)
{
    unsigned char ci = (unsigned char)c;
    if (ci < 0x20 || ci > 0x7F) ci = '?';
    const unsigned char *glyph = font8x8[ci - 0x20];

    int px = col * FONT_WIDTH;
    int py = row * FONT_HEIGHT;
    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        unsigned char bits = glyph[gy];
        unsigned int *line =
            (unsigned int *)(fb_base + (py + gy) * fb_pitch + px * 4);
        for (int gx = 0; gx < FONT_WIDTH; gx++) {
            line[gx] = (bits & (0x80 >> gx)) ? color_fg : color_bg;
        }
    }
}

static void scroll_one_row(void)
{
    /* Move every row up by FONT_HEIGHT pixels, then clear the bottom. */
    for (unsigned int y = 0; y < fb_height - FONT_HEIGHT; y++) {
        unsigned int *dst = (unsigned int *)(fb_base + y * fb_pitch);
        unsigned int *src =
            (unsigned int *)(fb_base + (y + FONT_HEIGHT) * fb_pitch);
        for (unsigned int x = 0; x < fb_width; x++) dst[x] = src[x];
    }
    for (unsigned int y = fb_height - FONT_HEIGHT; y < fb_height; y++) {
        unsigned int *row = (unsigned int *)(fb_base + y * fb_pitch);
        for (unsigned int x = 0; x < fb_width; x++) row[x] = color_bg;
    }
}

void screen_putc(char c)
{
    if (!fb_ready) return;

    if (c == '\r') { cursor_col = 0; return; }
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
        if (cursor_row >= ROWS) {
            scroll_one_row();
            cursor_row = ROWS - 1;
        }
        return;
    }
    if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            draw_glyph(cursor_col, cursor_row, ' ');
        }
        return;
    }
    if (cursor_col >= COLS) {
        cursor_col = 0;
        cursor_row++;
        if (cursor_row >= ROWS) {
            scroll_one_row();
            cursor_row = ROWS - 1;
        }
    }
    draw_glyph(cursor_col, cursor_row, c);
    cursor_col++;
}

void screen_puts(const char *s)
{
    while (*s) screen_putc(*s++);
}

/* ===================================================================
 * Drawing primitives for the window system (wm.c).
 *
 * All coordinates are pixels relative to the framebuffer's top-left.
 * No clipping: caller is responsible for staying within the screen
 * (fb_width × fb_height).  When fb_ready is false (video_init failed
 * or hasn't been called yet) every primitive becomes a no-op so
 * builds without HDMI just silently skip drawing.
 * ===================================================================
 */

unsigned int video_screen_width(void)  { return fb_width;  }
unsigned int video_screen_height(void) { return fb_height; }

void fill_rect(int x, int y, int w, int h, unsigned int color)
{
    if (!fb_ready) return;
    for (int dy = 0; dy < h; dy++) {
        unsigned int *row =
            (unsigned int *)(fb_base + (y + dy) * fb_pitch + x * 4);
        for (int dx = 0; dx < w; dx++) row[dx] = color;
    }
}

void draw_rect(int x, int y, int w, int h, unsigned int color)
{
    if (!fb_ready) return;
    /* top + bottom edges */
    unsigned int *top =
        (unsigned int *)(fb_base + y * fb_pitch + x * 4);
    unsigned int *bot =
        (unsigned int *)(fb_base + (y + h - 1) * fb_pitch + x * 4);
    for (int dx = 0; dx < w; dx++) { top[dx] = color; bot[dx] = color; }
    /* left + right edges */
    for (int dy = 0; dy < h; dy++) {
        *(unsigned int *)(fb_base + (y + dy) * fb_pitch + x * 4)           = color;
        *(unsigned int *)(fb_base + (y + dy) * fb_pitch + (x + w - 1) * 4) = color;
    }
}

void draw_glyph_at(int px, int py, char c,
                   unsigned int fg, unsigned int bg)
{
    if (!fb_ready) return;
    unsigned char ci = (unsigned char)c;
    if (ci < 0x20 || ci > 0x7F) ci = '?';
    const unsigned char *glyph = font8x8[ci - 0x20];
    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        unsigned char bits = glyph[gy];
        unsigned int *line =
            (unsigned int *)(fb_base + (py + gy) * fb_pitch + px * 4);
        for (int gx = 0; gx < FONT_WIDTH; gx++) {
            line[gx] = (bits & (0x80 >> gx)) ? fg : bg;
        }
    }
}

void draw_string_at(int px, int py, const char *s,
                    unsigned int fg, unsigned int bg)
{
    if (!fb_ready) return;
    while (*s) {
        draw_glyph_at(px, py, *s, fg, bg);
        px += FONT_WIDTH;
        s++;
    }
}

/* Busy-wait `ms` milliseconds based on the AArch64 generic timer.
 * CNTFRQ_EL0 returns the timer frequency in Hz (usually 54 MHz on
 * Pi 4 / Pi 5 / QEMU virt -cpu cortex-a76).  Used by wm_run() between
 * frame redraws for animation pacing. */
void delay_ms(unsigned int ms)
{
    unsigned long freq, start, now, target;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    target = (freq / 1000UL) * (unsigned long)ms;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));
    do {
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
    } while (now - start < target);
}
