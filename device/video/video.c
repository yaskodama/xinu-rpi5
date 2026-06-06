// device/video/video.c — HDMI framebuffer + 8x8 text console.

#include "video.h"
#include "mbox.h"
#include "memory.h"   /* getmem() for the optional double-buffer */

/* 16-byte aligned request buffer for the framebuffer-allocation
 * property tag sequence.  The exact tag IDs and layout are the
 * standard VC mailbox property interface, unchanged from Pi 2/3/4. */
static volatile unsigned int __attribute__((aligned(16))) mbox_buf[36];

static volatile unsigned char *fb_base;     /* visible HDMI framebuffer  */
static volatile unsigned char *fb_draw;     /* current render target     */
static volatile unsigned char *fb_back;     /* off-screen buffer (or 0)  */
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
    fb_draw   = fb_base;        /* draw direct until a back buffer is set */
    fb_back   = 0;
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
            (unsigned int *)(fb_draw + (py + gy) * fb_pitch + px * 4);
        for (int gx = 0; gx < FONT_WIDTH; gx++) {
            line[gx] = (bits & (0x80 >> gx)) ? color_fg : color_bg;
        }
    }
}

static void scroll_one_row(void)
{
    /* Move every row up by FONT_HEIGHT pixels, then clear the bottom. */
    for (unsigned int y = 0; y < fb_height - FONT_HEIGHT; y++) {
        unsigned int *dst = (unsigned int *)(fb_draw + y * fb_pitch);
        unsigned int *src =
            (unsigned int *)(fb_draw + (y + FONT_HEIGHT) * fb_pitch);
        for (unsigned int x = 0; x < fb_width; x++) dst[x] = src[x];
    }
    for (unsigned int y = fb_height - FONT_HEIGHT; y < fb_height; y++) {
        unsigned int *row = (unsigned int *)(fb_draw + y * fb_pitch);
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

/* ---- double buffering (anti-flicker) ----
 * The window manager redraws the whole screen every frame, including a
 * full background wipe.  Drawing that straight to the visible HDMI
 * framebuffer makes the "wipe then redraw" visible as flicker.  Instead
 * we allocate an off-screen buffer the size of the framebuffer, point
 * all rendering at it, and copy the finished frame to the visible
 * buffer in one pass via video_present().  If the allocation fails we
 * just keep drawing direct (unchanged behaviour, still flickers). */
int video_enable_backbuffer(void)
{
    if (!fb_ready) return 0;
    if (fb_back)   { fb_draw = fb_back; return 1; }   /* already enabled */
    void *p = getmem((unsigned long)fb_height * fb_pitch);
    if (!p) return 0;                  /* no memory — draw direct */
    fb_back = (volatile unsigned char *)p;
    fb_draw = fb_back;
    return 1;
}

void video_present(void)
{
    if (!fb_ready || fb_draw == fb_base) return;   /* nothing to flip */
    /* 64-bit, 8x-unrolled copy.  Memory is uncached on this port, so the
     * full-frame flip dominates frame time; wider bursts + fewer loop
     * iterations roughly halve it, which is what the mouse cursor needs to
     * look smooth (the flip rate caps how often cursor motion reaches HDMI). */
    volatile unsigned long *dst = (volatile unsigned long *)fb_base;
    volatile unsigned long *src = (volatile unsigned long *)fb_draw;
    unsigned int n = (fb_height * fb_pitch) / 8;
    unsigned int i = 0;
    for (; i + 8 <= n; i += 8) {
        dst[i+0]=src[i+0]; dst[i+1]=src[i+1]; dst[i+2]=src[i+2]; dst[i+3]=src[i+3];
        dst[i+4]=src[i+4]; dst[i+5]=src[i+5]; dst[i+6]=src[i+6]; dst[i+7]=src[i+7];
    }
    for (; i < n; i++) dst[i] = src[i];
}

/* Viewport offset: subtracted from every virtual-coord input
 * before it touches the framebuffer.  All primitives also clip
 * to [0, fb_width) × [0, fb_height) so off-screen geometry
 * (e.g. a window that scrolls partly past the right edge) can't
 * corrupt memory past the framebuffer. */
static int view_x = 0;
static int view_y = 0;

void video_set_viewport(int x, int y) { view_x = x; view_y = y; }
int  video_viewport_x(void) { return view_x; }
int  video_viewport_y(void) { return view_y; }

void fill_rect(int x, int y, int w, int h, unsigned int color)
{
    if (!fb_ready) return;

    int sx = x - view_x;
    int sy = y - view_y;
    if (sx < 0) { w += sx; sx = 0; }
    if (sy < 0) { h += sy; sy = 0; }
    if (sx + w > (int)fb_width)  w = (int)fb_width  - sx;
    if (sy + h > (int)fb_height) h = (int)fb_height - sy;
    if (w <= 0 || h <= 0) return;

    /* 64-bit pair writes (two pixels at a time) — halves the store count for
     * big fills like the per-frame desktop wipe (memory is uncached here). */
    unsigned long pair = ((unsigned long)color << 32) | color;
    for (int dy = 0; dy < h; dy++) {
        unsigned int *row =
            (unsigned int *)(fb_draw + (sy + dy) * fb_pitch + sx * 4);
        int dx = 0;
        if (((unsigned long)row & 7) && dx < w) { row[dx] = color; dx++; }  /* align to 8 */
        unsigned long *p = (unsigned long *)(row + dx);
        int pairs = (w - dx) >> 1;
        for (int i = 0; i < pairs; i++) p[i] = pair;
        for (dx += pairs * 2; dx < w; dx++) row[dx] = color;
    }
}

void draw_rect(int x, int y, int w, int h, unsigned int color)
{
    /* Express the four edges as 1-px-thick fill_rects — they
     * already handle viewport + clipping uniformly. */
    fill_rect(x,         y,         w, 1, color);   /* top    */
    fill_rect(x,         y + h - 1, w, 1, color);   /* bottom */
    fill_rect(x,         y,         1, h, color);   /* left   */
    fill_rect(x + w - 1, y,         1, h, color);   /* right  */
}

void draw_glyph_at(int px, int py, char c,
                   unsigned int fg, unsigned int bg)
{
    if (!fb_ready) return;
    unsigned char ci = (unsigned char)c;
    if (ci < 0x20 || ci > 0x7F) ci = '?';
    const unsigned char *glyph = font8x8[ci - 0x20];

    int sx = px - view_x;
    int sy = py - view_y;
    /* Trivial reject if the entire glyph cell is off-screen. */
    if (sx >= (int)fb_width || sy >= (int)fb_height) return;
    if (sx + FONT_WIDTH <= 0 || sy + FONT_HEIGHT <= 0) return;

    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        int rsy = sy + gy;
        if (rsy < 0 || rsy >= (int)fb_height) continue;
        unsigned char bits = glyph[gy];
        unsigned int *line =
            (unsigned int *)(fb_draw + rsy * fb_pitch);
        for (int gx = 0; gx < FONT_WIDTH; gx++) {
            int rsx = sx + gx;
            if (rsx < 0 || rsx >= (int)fb_width) continue;
            line[rsx] = (bits & (0x80 >> gx)) ? fg : bg;
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
