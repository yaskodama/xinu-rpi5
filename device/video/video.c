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
    volatile unsigned int *dst = (volatile unsigned int *)fb_base;
    volatile unsigned int *src = (volatile unsigned int *)fb_draw;
    unsigned int n = (fb_height * fb_pitch) / 4;
    for (unsigned int i = 0; i < n; i++) dst[i] = src[i];
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

    for (int dy = 0; dy < h; dy++) {
        unsigned int *row =
            (unsigned int *)(fb_draw + (sy + dy) * fb_pitch + sx * 4);
        for (int dx = 0; dx < w; dx++) row[dx] = color;
    }
}

/* ===================================================================
 *  Actor graphics canvas.  AIPL actors append line/circle commands
 *  (gfx_line / gfx_circle); the Graphics window replays the whole
 *  list every frame via gfx_render() so the drawing survives the wm's
 *  per-frame wipe.  Coordinates are window-content-relative; rendering
 *  clips to the window's content rect (and the framebuffer).
 * ===================================================================
 */
struct gfx_cmd { unsigned char type; int a, b, c, d; unsigned int color; };
#define GFX_MAX 1024
static struct gfx_cmd g_gfx[GFX_MAX];
static int g_gfx_n;
static const unsigned int gfx_palette[8] = {
    0xFF101010U,  /* 0 near-black */ 0xFFFF5050U, /* 1 red    */
    0xFF50FF50U,  /* 2 green      */ 0xFF5090FFU, /* 3 blue   */
    0xFFFFFF50U,  /* 4 yellow     */ 0xFF50FFFFU, /* 5 cyan   */
    0xFFFF50FFU,  /* 6 magenta    */ 0xFFFFFFFFU, /* 7 white  */
};
static unsigned int gfx_col(int idx) { return gfx_palette[(unsigned)idx & 7]; }

/* one pixel, clipped to the content rect [clx,cly,clw,clh] AND the screen */
static void gfx_pix(int x, int y, unsigned int color,
                    int clx, int cly, int clw, int clh)
{
    if (x < clx || y < cly || x >= clx + clw || y >= cly + clh) return;
    int sx = x - view_x, sy = y - view_y;
    if (sx < 0 || sy < 0 || sx >= (int)fb_width || sy >= (int)fb_height) return;
    *((unsigned int *)(fb_draw + sy * fb_pitch) + sx) = color;
}

static int iabs(int v) { return v < 0 ? -v : v; }

static void gfx_seg(int x0, int y0, int x1, int y1, unsigned int color,
                    int clx, int cly, int clw, int clh)
{
    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (int guard = 0; guard < 8192; guard++) {        /* bound: never spin */
        gfx_pix(x0, y0, color, clx, cly, clw, clh);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void gfx_ring(int cx, int cy, int r, unsigned int color,
                     int clx, int cly, int clw, int clh)
{
    if (r < 0) r = -r;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pix(cx + x, cy + y, color, clx, cly, clw, clh);
        gfx_pix(cx + y, cy + x, color, clx, cly, clw, clh);
        gfx_pix(cx - y, cy + x, color, clx, cly, clw, clh);
        gfx_pix(cx - x, cy + y, color, clx, cly, clw, clh);
        gfx_pix(cx - x, cy - y, color, clx, cly, clw, clh);
        gfx_pix(cx - y, cy - x, color, clx, cly, clw, clh);
        gfx_pix(cx + y, cy - x, color, clx, cly, clw, clh);
        gfx_pix(cx + x, cy - y, color, clx, cly, clw, clh);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void gfx_clear(void) { g_gfx_n = 0; }

void gfx_line(int x0, int y0, int x1, int y1, int color)
{
    if (g_gfx_n >= GFX_MAX) return;
    struct gfx_cmd *c = &g_gfx[g_gfx_n++];
    c->type = 1; c->a = x0; c->b = y0; c->c = x1; c->d = y1; c->color = gfx_col(color);
}

void gfx_circle(int cx, int cy, int r, int color)
{
    if (g_gfx_n >= GFX_MAX) return;
    struct gfx_cmd *c = &g_gfx[g_gfx_n++];
    c->type = 2; c->a = cx; c->b = cy; c->c = r; c->d = 0; c->color = gfx_col(color);
}

/* ---- 3D wireframe wine glass (a solid of revolution) -------------------
 * The bare-metal JIT has no libm, so a fixed-point sine table (sin*1024 for
 * 0..90 deg) + symmetry give isin/icos.  gfx_glass(ax,ay,az) builds the glass
 * fresh each call: a profile curve (radius vs height) revolved into a ring grid
 * (GLASS_M heights x GLASS_N angles), then tumbled by rotating each vertex about
 * all three axes (ay folds into the revolution = spin about Y; then about X by
 * ax, then about Z by az), projected orthographically, then drawn as parallels
 * (rings) + meridians (verticals).  One /actor/send drives a whole frame. */
static const short sintab[91] = {
       0,   18,   36,   54,   71,   89,  107,  125,  143,  160,
     178,  195,  213,  230,  248,  265,  282,  299,  316,  333,
     350,  367,  384,  400,  416,  433,  449,  465,  481,  496,
     512,  527,  543,  558,  573,  587,  602,  616,  630,  644,
     658,  672,  685,  698,  711,  724,  737,  749,  761,  773,
     784,  796,  807,  818,  828,  839,  849,  859,  868,  878,
     887,  896,  904,  912,  920,  928,  935,  943,  949,  956,
     962,  968,  974,  979,  984,  989,  994,  998, 1002, 1005,
    1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024,
    1024,
};
static int isin(int deg)
{
    deg %= 360; if (deg < 0) deg += 360;
    if (deg <=  90) return  sintab[deg];
    if (deg <= 180) return  sintab[180 - deg];
    if (deg <= 270) return -sintab[deg - 180];
    return -sintab[360 - deg];
}
static int icos(int deg) { return isin(deg + 90); }

#define GLASS_M 11        /* profile points (rim -> base)        */
#define GLASS_N 16        /* angular segments around the axis    */

void gfx_glass(int ax, int ay, int az)
{
    static const short gr[GLASS_M] = { 72, 71, 62, 48, 30, 12,  5,   5,   8,  45,  60 };
    static const short gh[GLASS_M] = {150,120, 90, 60, 35, 10,-20, -90,-125,-148,-155 };
    const int cx = 185, cy = 370;          /* centre, window-content coords  */
    const int cxr = icos(ax), sxr = isin(ax);   /* rotation about X (degrees) */
    const int czr = icos(az), szr = isin(az);   /* rotation about Z (degrees) */
    int px[GLASS_M * GLASS_N], py[GLASS_M * GLASS_N];

    gfx_clear();
    for (int i = 0; i < GLASS_M; i++) {
        for (int j = 0; j < GLASS_N; j++) {
            int th = (j * 360) / GLASS_N + ay;          /* revolve + spin about Y */
            int x  = gr[i] * icos(th) / 1024;
            int y  = gh[i];
            int z  = gr[i] * isin(th) / 1024;
            int y1 = (y  * cxr - z  * sxr) / 1024;       /* rotate about X */
            int z1 = (y  * sxr + z  * cxr) / 1024;
            int x2 = (x  * czr - y1 * szr) / 1024;       /* rotate about Z */
            int y2 = (x  * szr + y1 * czr) / 1024;
            (void)z1;                                    /* depth unused (orthographic) */
            px[i * GLASS_N + j] = cx + x2;
            py[i * GLASS_N + j] = cy - y2;
        }
    }
    /* parallels: the ring at each profile height (cyan) */
    for (int i = 0; i < GLASS_M; i++)
        for (int j = 0; j < GLASS_N; j++) {
            int a = i * GLASS_N + j, b = i * GLASS_N + (j + 1) % GLASS_N;
            gfx_line(px[a], py[a], px[b], py[b], 5);
        }
    /* meridians: the profile curve at each angle (yellow) */
    for (int j = 0; j < GLASS_N; j++)
        for (int i = 0; i < GLASS_M - 1; i++) {
            int a = i * GLASS_N + j, b = (i + 1) * GLASS_N + j;
            gfx_line(px[a], py[a], px[b], py[b], 4);
        }
}

/* Replay the command list into a window content area at (ox,oy) of size w×h.
 * Command coordinates are relative to (ox,oy). */
void gfx_render(int ox, int oy, int w, int h)
{
    if (!fb_ready) return;
    for (int i = 0; i < g_gfx_n; i++) {
        struct gfx_cmd *c = &g_gfx[i];
        if (c->type == 1)
            gfx_seg(ox + c->a, oy + c->b, ox + c->c, oy + c->d, c->color, ox, oy, w, h);
        else if (c->type == 2)
            gfx_ring(ox + c->a, oy + c->b, c->c, c->color, ox, oy, w, h);
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

/* Current text magnification for draw_string_at — the wm sets this to the
 * window's font_scale around each draw_content() so existing 1x-written
 * windows scale their glyphs without changing every call (they still must
 * scale their own line/column spacing).  Default 1. */
static int g_text_scale = 1;
void video_set_text_scale(int s) { g_text_scale = (s < 1) ? 1 : (s > 4 ? 4 : s); }
int  video_text_scale(void)      { return g_text_scale; }

void draw_string_at(int px, int py, const char *s,
                    unsigned int fg, unsigned int bg)
{
    if (!fb_ready) return;
    int sc = g_text_scale;
    while (*s) {
        draw_glyph_scaled(px, py, *s, fg, bg, sc);
        px += FONT_WIDTH * sc;
        s++;
    }
}

/* Magnified glyph: each 8x8 font pixel becomes a scale x scale block
 * (nearest-neighbour).  scale<=1 falls back to the 1x path. */
void draw_glyph_scaled(int px, int py, char c,
                       unsigned int fg, unsigned int bg, int scale)
{
    if (!fb_ready) return;
    if (scale <= 1) { draw_glyph_at(px, py, c, fg, bg); return; }
    unsigned char ci = (unsigned char)c;
    if (ci < 0x20 || ci > 0x7F) ci = '?';
    const unsigned char *glyph = font8x8[ci - 0x20];

    int sx = px - view_x;
    int sy = py - view_y;
    if (sx >= (int)fb_width || sy >= (int)fb_height) return;
    if (sx + FONT_WIDTH * scale <= 0 || sy + FONT_HEIGHT * scale <= 0) return;

    for (int gy = 0; gy < FONT_HEIGHT; gy++) {
        unsigned char bits = glyph[gy];
        for (int sj = 0; sj < scale; sj++) {
            int rsy = sy + gy * scale + sj;
            if (rsy < 0 || rsy >= (int)fb_height) continue;
            unsigned int *line = (unsigned int *)(fb_draw + rsy * fb_pitch);
            for (int gx = 0; gx < FONT_WIDTH; gx++) {
                unsigned int col = (bits & (0x80 >> gx)) ? fg : bg;
                for (int si = 0; si < scale; si++) {
                    int rsx = sx + gx * scale + si;
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
    if (scale < 1) scale = 1;
    while (*s) {
        draw_glyph_scaled(px, py, *s, fg, bg, scale);
        px += FONT_WIDTH * scale;
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
