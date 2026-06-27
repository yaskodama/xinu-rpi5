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
/* Text-console glyph scale: each 8x8 font pixel is drawn as a FONT_SCALE x
 * FONT_SCALE block, so the on-screen character cell is CELL_W x CELL_H.  Bumped
 * from 1 to 3 for readability (the minimal "OS #1" shell console lives here). */
#define FONT_SCALE  3
#define CELL_W  (FONT_WIDTH  * FONT_SCALE)
#define CELL_H  (FONT_HEIGHT * FONT_SCALE)
#define COLS  (SCREEN_WIDTH  / CELL_W)
#define ROWS  (SCREEN_HEIGHT / CELL_H)

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

    int px = col * CELL_W;
    int py = row * CELL_H;
    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        unsigned char bits = glyph[gy];
        for (int sy = 0; sy < FONT_SCALE; sy++) {
            unsigned int *line =
                (unsigned int *)(fb_draw + (py + gy * FONT_SCALE + sy) * fb_pitch + px * 4);
            for (int gx = 0; gx < FONT_WIDTH; gx++) {
                unsigned int c32 = (bits & (0x80 >> gx)) ? color_fg : color_bg;
                for (int sx = 0; sx < FONT_SCALE; sx++) line[gx * FONT_SCALE + sx] = c32;
            }
        }
    }
}

static void scroll_one_row(void)
{
    /* Move every row up by one character cell (CELL_H pixels), clear the bottom. */
    for (unsigned int y = 0; y < fb_height - CELL_H; y++) {
        unsigned int *dst = (unsigned int *)(fb_draw + y * fb_pitch);
        unsigned int *src =
            (unsigned int *)(fb_draw + (y + CELL_H) * fb_pitch);
        for (unsigned int x = 0; x < fb_width; x++) dst[x] = src[x];
    }
    for (unsigned int y = fb_height - CELL_H; y < fb_height; y++) {
        unsigned int *row = (unsigned int *)(fb_draw + y * fb_pitch);
        for (unsigned int x = 0; x < fb_width; x++) row[x] = color_bg;
    }
}

/* The boot log uses the text console (screen_putc).  Once the window manager is
 * running it owns the framebuffer (the shell window shows all text), so the text
 * console is invisible AND its 1080p full-frame scroll is very slow — every
 * uart_putc would otherwise pay that cost, making typing laggy and big command
 * output (e.g. `help`) wedge the HTTP handler.  Disable it when wm starts. */
static int g_screen_console = 1;
void screen_console_disable(void) { g_screen_console = 0; }

void screen_putc(char c)
{
    if (!fb_ready || !g_screen_console) return;

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

/* ---- decoupled hardware-style cursor ----------------------------------------
 * The slow full-frame flip caps how often the cursor reaches HDMI.  Instead we
 * keep the cursor OUT of the composited back buffer and stamp it straight onto
 * the visible framebuffer many times between flips, erasing its old spot by
 * copying the composited pixels back from the off-screen buffer.  This makes the
 * pointer move at the (fast) cursor-redraw rate, not the (slow) flip rate. */
#define VID_CURW 12
#define VID_CURH 12
static const unsigned char vid_cursor[VID_CURH][VID_CURW] = {
    {1,2,0,0,0,0,0,0,0,0,0,0},{1,1,2,0,0,0,0,0,0,0,0,0},
    {1,1,1,2,0,0,0,0,0,0,0,0},{1,1,1,1,2,0,0,0,0,0,0,0},
    {1,1,1,1,1,2,0,0,0,0,0,0},{1,1,1,1,1,1,2,0,0,0,0,0},
    {1,1,1,1,1,1,1,2,0,0,0,0},{1,1,1,1,1,1,1,1,2,0,0,0},
    {1,1,1,1,1,2,2,2,2,0,0,0},{1,1,2,1,1,2,0,0,0,0,0,0},
    {1,2,0,2,1,1,2,0,0,0,0,0},{2,0,0,0,2,1,1,2,0,0,0,0},
};
static volatile int vid_cur_x = -10000, vid_cur_y = -10000;   /* where it's stamped (ISR-updated) */
static volatile int vid_cur_vis = 0;
static volatile int vid_presenting = 0;              /* 1 while a flip is in progress */

static void vid_restore(int x, int y)                /* back-buffer pixels -> front */
{
    if (!fb_back) return;
    for (int r = 0; r < VID_CURH; r++) {
        int yy = y + r; if (yy < 0 || yy >= (int)fb_height) continue;
        for (int c = 0; c < VID_CURW; c++) {
            int xx = x + c; if (xx < 0 || xx >= (int)fb_width) continue;
            *(volatile unsigned int *)(fb_base + yy*fb_pitch + xx*4) =
            *(volatile unsigned int *)(fb_back + yy*fb_pitch + xx*4);
        }
    }
}

/* Stamp the cursor at (x,y) on the visible buffer, erasing the previous spot. */
void video_cursor_to_front(int x, int y, int visible)
{
    if (!fb_ready || !fb_back) return;
    /* No present-guard: stamping during the flip keeps the pointer moving the
     * whole frame instead of freezing for the ~10 ms flip.  present_hole skips
     * the cursor rect, so the worst case is a 1-frame flicker on a fast flick. */
    if (vid_cur_x > -10000) vid_restore(vid_cur_x, vid_cur_y);   /* erase old */
    vid_cur_vis = visible;
    if (visible) {
        for (int r = 0; r < VID_CURH; r++) {
            int yy = y + r; if (yy < 0 || yy >= (int)fb_height) continue;
            for (int c = 0; c < VID_CURW; c++) {
                int xx = x + c; if (xx < 0 || xx >= (int)fb_width) continue;
                unsigned char p = vid_cursor[r][c];
                if (!p) continue;
                *(volatile unsigned int *)(fb_base + yy*fb_pitch + xx*4) =
                    p == 1 ? 0xFFFFFFFFu : 0xFF000000u;
            }
        }
    }
    vid_cur_x = x; vid_cur_y = y;
}

/* Copy back-buffer rows [lo,hi) to the visible framebuffer, leaving a hole
 * around the live cursor rect.  Matches smp_range_fn so the per-frame flip can
 * be split across the 4 cores (see video_present_hole).  `core` is unused. */
/* Copy `n` 32-bit pixels d[0..n) <- s[0..n).  With the D-cache off the cost is
 * per-store, so we move 64 bits at a time (2 px) and unroll ×4 (32 B/iter) to
 * cut loop + store-instruction overhead roughly in half vs a uint32 loop. */
static inline void row_copy(unsigned int *d, const unsigned int *s, unsigned int n)
{
    unsigned long       *dd = (unsigned long *)d;
    const unsigned long *ss = (const unsigned long *)s;
    unsigned int n2 = n >> 1;            /* 64-bit words */
    unsigned int i = 0;
    for (; i + 4 <= n2; i += 4) {
        dd[i]   = ss[i];   dd[i+1] = ss[i+1];
        dd[i+2] = ss[i+2]; dd[i+3] = ss[i+3];
    }
    for (; i < n2; i++) dd[i] = ss[i];
    if (n & 1) d[n - 1] = s[n - 1];      /* odd trailing pixel (1920 is even) */
}

static long present_band(long lo, long hi, int core)
{
    (void)core;
    /* A small margin around the cursor so that even if the ISR nudges it between
     * rows, the live cursor still lands inside the skipped band. */
    const int MG = 10;
    for (long y = lo; y < hi; y++) {
        unsigned int *d = (unsigned int *)(fb_base + y*fb_pitch);
        unsigned int *s = (unsigned int *)(fb_draw + y*fb_pitch);
        /* Re-read the cursor position every row so the hole tracks it live. */
        int hx = vid_cur_x, hy = vid_cur_y;
        int has_hole = vid_cur_vis && hx > -10000;
        if (!has_hole || (int)y < hy - MG || (int)y >= hy + VID_CURH + MG) {
            row_copy(d, s, fb_width);                              /* whole row */
        } else {
            int x0 = hx - MG;                 if (x0 < 0) x0 = 0;
            int x1 = hx + VID_CURW + MG;      if (x1 > (int)fb_width) x1 = (int)fb_width;
            if (x0 > 0)                 row_copy(d, s, (unsigned)x0);
            if (x1 < (int)fb_width)     row_copy(d + x1, s + x1, fb_width - (unsigned)x1);
        }
    }
    return 0;
}

/* Flip the composed frame to HDMI but leave the live cursor rect untouched so a
 * directly-stamped cursor survives the flip (no per-frame cursor flicker).
 *
 * The copy is the per-frame bottleneck (a full 1920x1080x4 = ~8 MiB blit with
 * the D-cache off), so we fan it out across the worker-pool cores brought up by
 * smp_init() — each core blits a contiguous band of rows.  This roughly triples
 * the flip rate on the Pi 5's 4 A76 cores, which is what makes on-screen
 * keyboard echo feel smooth.  If SMP is not up, smp_parallel_sum() runs the
 * whole range inline on core 0 — identical to the old single-core flip. */
void video_present_hole(void)
{
    extern int  smp_cores_online(void);
    typedef long (*present_fn)(long, long, int);
    extern long smp_parallel_sum(present_fn fn, long n, int ncores);

    if (!fb_ready || fb_draw == fb_base) return;
    vid_presenting = 1;
    int nc = smp_cores_online();
    if (nc < 1) nc = 1;
    smp_parallel_sum(present_band, (long)fb_height, nc);
    vid_presenting = 0;
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

/* Bresenham line in virtual-desktop coords (viewport-relative, clipped to fb). */
void draw_line(int x0, int y0, int x1, int y1, unsigned int color)
{
    if (!fb_ready) return;
    x0 -= view_x; y0 -= view_y; x1 -= view_x; y1 -= view_y;
    int dx =  (x1 > x0 ? x1 - x0 : x0 - x1), sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < (int)fb_width && y0 >= 0 && y0 < (int)fb_height)
            *(volatile unsigned int *)(fb_draw + y0*fb_pitch + x0*4) = color;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
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

/* ===================================================================
 *  Scaled glyph / string drawing (BASIC window toolbar uses scale 1,
 *  but the helper is general).  Coords are virtual-desktop pixels —
 *  viewport offset + clip-to-fb applied exactly like draw_glyph_at.
 * =================================================================== */
void draw_glyph_scaled(int px, int py, char c,
                       unsigned int fg, unsigned int bg, int scale)
{
    if (!fb_ready) return;
    if (scale < 1) scale = 1;
    unsigned char ci = (unsigned char)c;
    if (ci < 0x20 || ci > 0x7F) ci = '?';
    const unsigned char *glyph = font8x8[ci - 0x20];

    int sx0 = px - view_x;
    int sy0 = py - view_y;
    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        unsigned char bits = glyph[gy];
        for (int sy = 0; sy < scale; sy++) {
            int rsy = sy0 + gy * scale + sy;
            if (rsy < 0 || rsy >= (int)fb_height) continue;
            unsigned int *line = (unsigned int *)(fb_draw + rsy * fb_pitch);
            for (int gx = 0; gx < FONT_WIDTH; gx++) {
                unsigned int col = (bits & (0x80 >> gx)) ? fg : bg;
                for (int sxx = 0; sxx < scale; sxx++) {
                    int rsx = sx0 + gx * scale + sxx;
                    if (rsx < 0 || rsx >= (int)fb_width) continue;
                    line[rsx] = col;
                }
            }
        }
    }
}

void draw_string_scaled(int px, int py, const char *s,
                        unsigned int fg, unsigned int bg, int scale)
{
    if (!fb_ready) return;
    if (scale < 1) scale = 1;
    while (*s) {
        draw_glyph_scaled(px, py, *s, fg, bg, scale);
        px += FONT_WIDTH * scale;
        s++;
    }
}

/* ===================================================================
 *  BASIC graphics canvas.  The BASIC window's LINE / CIRCLE / PLOT
 *  statements append commands to a display list (bgfx_line/circle);
 *  basicwin replays the whole list every frame via bgfx_render() so
 *  the drawing survives the wm's per-frame wipe.  Coordinates are
 *  window-content-relative; rendering clips to the window's content
 *  rect (and the framebuffer).  Mirrors the actor canvas in rpi4 but
 *  kept BASIC-only here (the Graphics window owns its own renderer).
 * =================================================================== */
struct bgfx_cmd { unsigned char type; int a, b, c, d; unsigned int color; };
#define BGFX_MAX 1024
static struct bgfx_cmd g_bgfx[BGFX_MAX];
static int g_bgfx_n;
static const unsigned int bgfx_palette[8] = {
    0xFF101010U,  /* 0 near-black */ 0xFFFF5050U, /* 1 red    */
    0xFF50FF50U,  /* 2 green      */ 0xFF5090FFU, /* 3 blue   */
    0xFFFFFF50U,  /* 4 yellow     */ 0xFF50FFFFU, /* 5 cyan   */
    0xFFFF50FFU,  /* 6 magenta    */ 0xFFFFFFFFU, /* 7 white  */
};
static unsigned int bgfx_col(int idx) { return bgfx_palette[(unsigned)idx & 7]; }
static int bgfx_iabs(int v) { return v < 0 ? -v : v; }

/* one pixel, clipped to the content rect [clx,cly,clw,clh] AND the screen */
static void bgfx_pix(int x, int y, unsigned int color,
                     int clx, int cly, int clw, int clh)
{
    if (x < clx || y < cly || x >= clx + clw || y >= cly + clh) return;
    int sx = x - view_x, sy = y - view_y;
    if (sx < 0 || sy < 0 || sx >= (int)fb_width || sy >= (int)fb_height) return;
    *((unsigned int *)(fb_draw + sy * fb_pitch) + sx) = color;
}

static void bgfx_seg(int x0, int y0, int x1, int y1, unsigned int color,
                     int clx, int cly, int clw, int clh)
{
    int dx = bgfx_iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -bgfx_iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (int guard = 0; guard < 8192; guard++) {        /* bound: never spin */
        bgfx_pix(x0, y0, color, clx, cly, clw, clh);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void bgfx_ring(int cx, int cy, int r, unsigned int color,
                      int clx, int cly, int clw, int clh)
{
    if (r < 0) r = -r;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        bgfx_pix(cx + x, cy + y, color, clx, cly, clw, clh);
        bgfx_pix(cx + y, cy + x, color, clx, cly, clw, clh);
        bgfx_pix(cx - y, cy + x, color, clx, cly, clw, clh);
        bgfx_pix(cx - x, cy + y, color, clx, cly, clw, clh);
        bgfx_pix(cx - x, cy - y, color, clx, cly, clw, clh);
        bgfx_pix(cx - y, cy - x, color, clx, cly, clw, clh);
        bgfx_pix(cx + y, cy - x, color, clx, cly, clw, clh);
        bgfx_pix(cx + x, cy - y, color, clx, cly, clw, clh);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void bgfx_clear(void) { g_bgfx_n = 0; }
void bgfx_line(int x0, int y0, int x1, int y1, int color)
{
    if (g_bgfx_n >= BGFX_MAX) return;
    struct bgfx_cmd *c = &g_bgfx[g_bgfx_n++];
    c->type = 1; c->a = x0; c->b = y0; c->c = x1; c->d = y1; c->color = bgfx_col(color);
}
void bgfx_circle(int cx, int cy, int r, int color)
{
    if (g_bgfx_n >= BGFX_MAX) return;
    struct bgfx_cmd *c = &g_bgfx[g_bgfx_n++];
    c->type = 2; c->a = cx; c->b = cy; c->c = r; c->d = 0; c->color = bgfx_col(color);
}

/* Replay the BASIC display list, offset into the content rect (ox,oy,w,h). */
void bgfx_render(int ox, int oy, int w, int h)
{
    for (int i = 0; i < g_bgfx_n; i++) {
        const struct bgfx_cmd *c = &g_bgfx[i];
        if (c->type == 1)
            bgfx_seg(ox + c->a, oy + c->b, ox + c->c, oy + c->d, c->color, ox, oy, w, h);
        else if (c->type == 2)
            bgfx_ring(ox + c->a, oy + c->b, c->c, c->color, ox, oy, w, h);
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
