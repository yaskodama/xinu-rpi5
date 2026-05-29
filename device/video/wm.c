// device/video/wm.c — window manager: chrome + redraw loop.

#include "wm.h"
#include "video.h"

#define DESKTOP_BG     0xFF003366U   /* dark navy "desktop"          */
#define DEFAULT_FPS    20            /* 1 frame every 50 ms          */

static window_t *wm_head;
static void    (*wm_tick)(void);

/* Cursor overlay state — repainted on top of all windows every
 * frame so it never disappears under another redraw.  Cursor
 * coordinates are in *screen* space, not virtual desktop, so the
 * cursor stays anchored to the display when the viewport pans. */
static int cursor_x = 320;
static int cursor_y = 240;
static int cursor_visible = 1;

/* Viewport state — top-left corner of the visible camera inside
 * the WM_DESKTOP_W × WM_DESKTOP_H virtual desktop. */
static int vp_x = 0;
static int vp_y = 0;
/* Autopan defaults to OFF — the boot log needs to stay readable
 * in the shell window during early debugging.  The `autopan on`
 * shell command (re-)enables the demo cycle once input works. */
static int autopan_on = 0;

static void clamp_viewport(int sw, int sh)
{
    int max_x = WM_DESKTOP_W - sw;
    int max_y = WM_DESKTOP_H - sh;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    if (vp_x < 0) vp_x = 0;
    if (vp_y < 0) vp_y = 0;
    if (vp_x > max_x) vp_x = max_x;
    if (vp_y > max_y) vp_y = max_y;
}

void wm_pan(int dx, int dy)
{
    vp_x += dx;
    vp_y += dy;
    /* Clamp on next frame using the real screen size. */
}

void wm_set_viewport(int x, int y) { vp_x = x; vp_y = y; }
int  wm_view_x(void) { return vp_x; }
int  wm_view_y(void) { return vp_y; }

void wm_set_autopan(int on) { autopan_on = on ? 1 : 0; }

void wm_set_tick(void (*fn)(void))
{
    wm_tick = fn;
}

void wm_cursor_set(int x, int y, int visible)
{
    cursor_x = x;
    cursor_y = y;
    cursor_visible = visible;
}

/* 12×12 arrow cursor sprite.  '#' = white, '.' = black border,
 * ' ' = transparent.  Anchor (hot-spot) is top-left. */
static const char cursor_sprite[12][12] = {
    {'#','.',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'#','#','.',' ',' ',' ',' ',' ',' ',' ',' ',' '},
    {'#','#','#','.',' ',' ',' ',' ',' ',' ',' ',' '},
    {'#','#','#','#','.',' ',' ',' ',' ',' ',' ',' '},
    {'#','#','#','#','#','.',' ',' ',' ',' ',' ',' '},
    {'#','#','#','#','#','#','.',' ',' ',' ',' ',' '},
    {'#','#','#','#','#','#','#','.',' ',' ',' ',' '},
    {'#','#','#','#','#','#','#','#','.',' ',' ',' '},
    {'#','#','#','#','#','.','.','.','.',' ',' ',' '},
    {'#','#','.','#','#','.',' ',' ',' ',' ',' ',' '},
    {'#','.',' ','.','#','#','.',' ',' ',' ',' ',' '},
    {'.',' ',' ',' ','.','#','#','.',' ',' ',' ',' '},
};

static void draw_cursor(void)
{
    if (!cursor_visible) return;
    /* Cursor is in screen coords — reset the viewport so the
     * sprite always renders 1:1 onto the physical display, then
     * restore it for the next frame's window draws. */
    int save_x = video_viewport_x();
    int save_y = video_viewport_y();
    video_set_viewport(0, 0);

    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();
    for (int dy = 0; dy < 12; dy++) {
        int py = cursor_y + dy;
        if (py < 0 || py >= sh) continue;
        for (int dx = 0; dx < 12; dx++) {
            int px = cursor_x + dx;
            if (px < 0 || px >= sw) continue;
            char c = cursor_sprite[dy][dx];
            if (c == '#')      fill_rect(px, py, 1, 1, 0xFFFFFFFFU);
            else if (c == '.') fill_rect(px, py, 1, 1, 0xFF000000U);
        }
    }

    video_set_viewport(save_x, save_y);
}

void wm_add(window_t *w)
{
    w->next = 0;
    if (wm_head == 0) {
        wm_head = w;
        return;
    }
    window_t *t = wm_head;
    while (t->next) t = t->next;
    t->next = w;
}

/* ---- runtime window geometry (driven by the AIPL layout designer) ---- */
static window_t *wm_nth(int idx)
{
    window_t *w = wm_head;
    while (w && idx-- > 0) w = w->next;
    return w;
}

int wm_window_count(void)
{
    int n = 0;
    for (window_t *w = wm_head; w; w = w->next) n++;
    return n;
}

int wm_window_move(int idx, int x, int y)
{
    window_t *w = wm_nth(idx);
    if (!w) return -1;
    if (x < 0) x = 0; if (x > WM_DESKTOP_W - 16) x = WM_DESKTOP_W - 16;
    if (y < 0) y = 0; if (y > WM_DESKTOP_H - 16) y = WM_DESKTOP_H - 16;
    w->x = x; w->y = y;        /* next frame redraws at the new spot */
    return 0;
}

int wm_window_resize(int idx, int wd, int ht)
{
    window_t *w = wm_nth(idx);
    if (!w) return -1;
    if (wd >= 24) w->width  = wd;   /* keep room for the titlebar/chrome */
    if (ht >= 24) w->height = ht;
    return 0;
}

int wm_window_name(int idx, char *out, int cap)
{
    window_t *w = wm_nth(idx);
    if (!w || cap <= 0) return -1;
    int i = 0;
    while (w->title[i] && i < cap - 1) { out[i] = w->title[i]; i++; }
    out[i] = 0;
    return i;
}

int wm_window_get(int idx, int *x, int *y, int *wd, int *ht)
{
    window_t *w = wm_nth(idx);
    if (!w) return -1;
    if (x)  *x  = w->x;
    if (y)  *y  = w->y;
    if (wd) *wd = w->width;
    if (ht) *ht = w->height;
    return 0;
}

int wm_window_font(int idx, int scale)
{
    window_t *w = wm_nth(idx);
    if (!w) return -1;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;          /* keep glyphs sane vs. the window size */
    w->font_scale = scale;
    return 0;
}

int wm_window_fontscale(int idx)
{
    window_t *w = wm_nth(idx);
    return (w && w->font_scale > 0) ? w->font_scale : 1;
}

static void draw_chrome(window_t *w)
{
    /* outer border */
    draw_rect(w->x, w->y, w->width, w->height, w->chrome_color);

    /* title bar background (one pixel inside the border) */
    fill_rect(w->x + 1, w->y + 1, w->width - 2, WM_TITLEBAR_H, w->title_bg);

    /* title text — left-aligned with a 4 px gutter */
    draw_string_at(w->x + 4, w->y + 2, w->title, w->title_fg, w->title_bg);

    /* separator under the title */
    fill_rect(w->x + 1, w->y + WM_TITLEBAR_H + 1,
              w->width - 2, 1, w->chrome_color);

    /* content background */
    int cy = w->y + WM_TITLEBAR_H + 2;
    int ch = w->height - WM_TITLEBAR_H - 3;
    fill_rect(w->x + 1, cy, w->width - 2, ch, w->content_bg);
}

void wm_run(void)
{
    unsigned int frame = 0;
    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();

    /* Render off-screen so the per-frame full-screen wipe + redraw never
     * shows as flicker; each finished frame is flipped to the visible
     * framebuffer in one pass via video_present().  Falls back to direct
     * drawing if the buffer can't be allocated. */
    video_enable_backbuffer();

    /* Initial desktop wipe (screen-space, viewport bypass). */
    video_set_viewport(0, 0);
    fill_rect(0, 0, sw, sh, DESKTOP_BG);

    for (;;) {
        if (wm_tick) wm_tick();

        /* Auto-pan demo: cycle the viewport through the four
         * corners of the virtual desktop on a ~24 s loop.  Until
         * USPi delivers real arrow-key / mouse-button input, this
         * is the only way to actually *see* the bigger desktop
         * scroll.  Phase boundaries: 0..120 right, 120..240 down,
         * 240..360 left, 360..480 up.  At 20 fps frame=480 ≈ 24 s. */
        if (autopan_on) {
            unsigned int phase = (frame / 120) & 3;
            int t = (int)(frame % 120);
            int max_x = WM_DESKTOP_W - sw;
            int max_y = WM_DESKTOP_H - sh;
            if (max_x < 0) max_x = 0;
            if (max_y < 0) max_y = 0;
            switch (phase) {
                case 0: vp_x = (max_x * t) / 120;       vp_y = 0;                       break;
                case 1: vp_x = max_x;                    vp_y = (max_y * t) / 120;      break;
                case 2: vp_x = max_x - (max_x * t)/120;  vp_y = max_y;                  break;
                case 3: vp_x = 0;                        vp_y = max_y - (max_y * t)/120;break;
            }
        }
        clamp_viewport(sw, sh);

        /* Repaint the visible screen with the desktop background
         * before any windows — the auto-pan shifts the camera so
         * stale pixels from the previous frame need to be cleared. */
        video_set_viewport(0, 0);
        fill_rect(0, 0, sw, sh, DESKTOP_BG);

        /* Now switch to the panned camera and draw all windows in
         * virtual desktop coordinates. */
        video_set_viewport(vp_x, vp_y);
        for (window_t *w = wm_head; w; w = w->next) {
            video_set_text_scale(1);          /* chrome/title always 1x */
            draw_chrome(w);
            if (w->draw_content) {
                /* Scale draw_string_at glyphs to this window's font size; the
                 * window's own draw fn scales its line/column spacing to match. */
                video_set_text_scale(w->font_scale > 0 ? w->font_scale : 1);
                w->draw_content(w, frame);
                video_set_text_scale(1);
            }
        }

        draw_cursor();   /* screen-space overlay, always on top */
        video_present(); /* flip the finished off-screen frame to HDMI */
        delay_ms(1000 / DEFAULT_FPS);
        frame++;
    }
}
