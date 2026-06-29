// device/video/wm.c — window manager: chrome + redraw loop.

#include "wm.h"
#include "video.h"

#define DESKTOP_BG     0xFF003366U   /* dark navy "desktop"          */
#define DEFAULT_FPS    60            /* cap; the loop runs as fast as the
                                       * redraw allows so the 100 Hz mouse pump
                                       * shows up smoothly (was 20 = choppy)   */
/* Between each (slow) full-frame flip, re-stamp the decoupled cursor this many
 * times at this spacing — the cursor then tracks the 100 Hz mouse pump instead
 * of the much slower window-compose rate. */
#define CURSOR_SUBFRAMES 8
#define CURSOR_SUBMS     2

static window_t *wm_head;
static int       s_force_wipe;          /* repaint the whole desktop next frame */
void wm_request_full_redraw(void) { s_force_wipe = 1; }

/* A single pending background-clear rectangle (virtual-desktop coords), applied
 * once at the start of the next compose.  Used while dragging/resizing a window
 * to erase its old footprint WITHOUT wiping the whole screen — far cheaper than
 * s_force_wipe, so the drag stays smooth.  Successive requests union together
 * so several mouse moves within one frame all get cleared. */
static int s_clr_pending, s_clr_x0, s_clr_y0, s_clr_x1, s_clr_y1;
void wm_clear_bg_rect(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (!s_clr_pending) { s_clr_x0 = x; s_clr_y0 = y; s_clr_x1 = x1; s_clr_y1 = y1; }
    else {
        if (x  < s_clr_x0) s_clr_x0 = x;
        if (y  < s_clr_y0) s_clr_y0 = y;
        if (x1 > s_clr_x1) s_clr_x1 = x1;
        if (y1 > s_clr_y1) s_clr_y1 = y1;
    }
    s_clr_pending = 1;
}
static void    (*wm_tick)(void);
static void    (*wm_overlay)(void);

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

/* Scroll the viewport over the virtual desktop by (dx,dy).  Bounds are clamped
 * by clamp_viewport() at the top of each frame, so out-of-range deltas are
 * harmless.  Used by the mouse edge-pan in xhci_mouse_event(). */
void wm_scroll(int dx, int dy) { vp_x += dx; vp_y += dy; }

void wm_set_autopan(int on) { autopan_on = on ? 1 : 0; }

void wm_set_tick(void (*fn)(void))
{
    wm_tick = fn;
}

void wm_set_overlay(void (*fn)(void))
{
    wm_overlay = fn;
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

/* Address a window by its add order (0-based), like the Pi 4 Layout actor's
 * win_move/win_resize builtins.  The Mac screen-designer ships geometry over
 * the debug UART and these reposition the live windows (the wm_run loop
 * repaints every frame, so changes show on the next frame). */
window_t *wm_nth(int id)
{
    window_t *w = wm_head;
    while (w && id-- > 0) w = w->next;
    return w;
}

void wm_move_window(int id, int x, int y)
{
    window_t *w = wm_nth(id);
    if (w) { w->x = x; w->y = y; }
}

void wm_resize_window(int id, int w_, int h_)
{
    window_t *w = wm_nth(id);
    if (!w) return;
    if (w_ >= 32) w->width  = w_;     /* keep at least a usable chrome size */
    if (h_ >= 24) w->height = h_;
}

/* The currently focused (selected) window, or NULL. */
window_t *wm_focused(void)
{
    for (window_t *w = wm_head; w; w = w->next) if (w->focused) return w;
    return 0;
}

/* Focus + raise the topmost window under a screen-space point (the cursor). */
window_t *wm_focus_at(int sx, int sy)
{
    int dx = sx + vp_x, dy = sy + vp_y;        /* screen -> virtual-desktop coords */
    window_t *hit = 0;
    for (window_t *w = wm_head; w; w = w->next)
        if (dx >= w->x && dx < w->x + w->width &&
            dy >= w->y && dy < w->y + w->height)
            hit = w;                           /* keep last == topmost in draw order */
    if (!hit) return 0;

    for (window_t *w = wm_head; w; w = w->next) w->focused = 0;
    hit->focused = 1;

    /* Raise: unlink and re-append so it draws last (on top). */
    if (hit->next) {
        if (wm_head == hit) wm_head = hit->next;
        else { window_t *p = wm_head; while (p->next && p->next != hit) p = p->next;
               if (p->next == hit) p->next = hit->next; }
        hit->next = 0;
        window_t *t = wm_head; while (t && t->next) t = t->next;
        if (t) t->next = hit; else wm_head = hit;
    }
    return hit;
}

/* Topmost window under a screen-space point, WITHOUT changing focus/raise. */
window_t *wm_window_at(int sx, int sy)
{
    int dx = sx + vp_x, dy = sy + vp_y;
    window_t *hit = 0;
    for (window_t *w = wm_head; w; w = w->next)
        if (dx >= w->x && dx < w->x + w->width &&
            dy >= w->y && dy < w->y + w->height)
            hit = w;                           /* last == topmost in draw order */
    return hit;
}

/* True if (vx,vy) — virtual-desktop coords — falls on `w`'s close button
 * (the red 'x' square at the top-left of the title bar). */
int wm_close_hit(window_t *w, int vx, int vy)
{
    if (!w) return 0;
    int bx = w->x + 1, by = w->y + 1, bw = WM_TITLEBAR_H;
    return vx >= bx && vx < bx + bw && vy >= by && vy < by + WM_TITLEBAR_H;
}

/* Is `w` currently in the window list (i.e. shown / interactive)? */
int wm_is_shown(window_t *w)
{
    for (window_t *p = wm_head; p; p = p->next) if (p == w) return 1;
    return 0;
}

/* Remove `w` from the window list (close it).  Focus passes to the new topmost
 * window.  The caller should arrange any owner cleanup (e.g. shell instance). */
void wm_remove(window_t *w)
{
    if (!w || !wm_head) return;
    if (wm_head == w) wm_head = w->next;
    else {
        window_t *p = wm_head;
        while (p->next && p->next != w) p = p->next;
        if (p->next == w) p->next = w->next;
    }
    w->next = 0;
    if (w->focused) {
        w->focused = 0;
        window_t *t = wm_head; while (t && t->next) t = t->next;   /* topmost */
        if (t) t->focused = 1;
    }
    s_force_wipe = 1;        /* erase the closed window's pixels next frame */
}

static void draw_chrome(window_t *w)
{
    /* Focused window gets a bright white border + lightened title bar. */
    unsigned int border = w->focused ? 0xFFFFFFFFu : w->chrome_color;
    unsigned int tbg    = w->focused ? (w->title_bg | 0x00505050u) : w->title_bg;

    /* outer border (2 px when focused for a clearer selection cue) */
    draw_rect(w->x, w->y, w->width, w->height, border);
    if (w->focused && w->width > 4 && w->height > 4)
        draw_rect(w->x + 1, w->y + 1, w->width - 2, w->height - 2, border);

    /* title bar background (one pixel inside the border) */
    fill_rect(w->x + 1, w->y + 1, w->width - 2, WM_TITLEBAR_H, tbg);

    /* close button: a red square with an 'x' at the top-left of the title bar.
     * Clicking it removes the window (see wm_close_hit / wm_remove). */
    {
        int bx = w->x + 1, by = w->y + 1, bw = WM_TITLEBAR_H;
        fill_rect(bx, by, bw, WM_TITLEBAR_H, 0xFFC83C2Cu);          /* red */
        draw_string_at(bx + (bw - FONT_WIDTH) / 2, by + 2, "x",
                       0xFFFFFFFFu, 0xFFC83C2Cu);
    }

    /* title text — to the right of the close button */
    draw_string_at(w->x + WM_TITLEBAR_H + 5, w->y + 2, w->title, w->title_fg, tbg);

    /* separator under the title */
    fill_rect(w->x + 1, w->y + WM_TITLEBAR_H + 1,
              w->width - 2, 1, w->chrome_color);

    /* content background */
    int cy = w->y + WM_TITLEBAR_H + 2;
    int ch = w->height - WM_TITLEBAR_H - 3;
    fill_rect(w->x + 1, cy, w->width - 2, ch, w->content_bg);

    /* resize grip: a small staircase mark in the bottom-right corner so the user
     * can see where to grab to resize the window. */
    {
        int gx = w->x + w->width - 3, gy = w->y + w->height - 3;
        unsigned int gc = w->focused ? 0xFFFFFFFFu : w->chrome_color;
        for (int i = 0; i < 3; i++)
            fill_rect(gx - i*4, gy - i*4, 3, 3, gc);
    }
}

/* WiFi status indicator in the bottom-right corner: a classic "fan" (dot + three
 * arcs) and, below it, the SSID and our own IP — like a desktop taskbar widget.
 * Green/white when associated + DHCP'd, gray otherwise.  Drawn in screen space. */
extern int  wifi_connected(void);
extern const char *wifi_ssid(void);
extern void wifi_ipaddr(unsigned char *o);
static int  wm_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void draw_wifi_icon(int sw, int sh)
{
    /* +-50 deg upward fan, sin/cos x1000 at -50,-30,-10,10,30,50 */
    static const int si[6] = { -766, -500, -174,  174,  500,  766 };
    static const int ci[6] = {  643,  866,  985,  985,  866,  643 };
    int connected = wifi_connected();
    unsigned int col = connected ? 0xFF2ECC55u : 0xFF707070u;        /* icon: green/gray */
    unsigned int txt = connected ? 0xFFFFFFFFu : 0xFF909090u;        /* text: white/gray */
    int cx = sw - 26, cy = sh - 46;      /* fan apex, raised to leave room for text */
    int a, k, th;
    /* clear a tall box in the corner (icon + two text lines) */
    fill_rect(sw - 184, sh - 76, 184, 76, DESKTOP_BG);
    fill_rect(cx - 2, cy - 2, 5, 5, col);                            /* the dot */
    for (a = 0; a < 3; a++) {                                        /* arcs r = 9/16/23 */
        int r = 9 + a * 7;
        for (th = 0; th < 2; th++) {                                 /* 2 px thick */
            int rr = r - th, px = -1, py = -1;
            for (k = 0; k < 6; k++) {
                int x = cx + (rr * si[k]) / 1000;
                int y = cy - (rr * ci[k]) / 1000;
                if (px >= 0) draw_line(px, py, x, y, col);
                px = x; py = y;
            }
        }
    }
    if (connected) {
        const char *ssid = wifi_ssid();
        unsigned char ip[4]; char ipbuf[16]; int n = 0, i, w;
        wifi_ipaddr(ip);
        for (i = 0; i < 4; i++) {                                    /* ip -> "a.b.c.d" */
            unsigned int v = ip[i]; char t[3]; int m = 0;
            if (i) ipbuf[n++] = '.';
            if (v == 0) t[m++] = '0'; else while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
            while (m--) ipbuf[n++] = t[m];
        }
        ipbuf[n] = 0;
        w = wm_strlen(ssid)  * 8; draw_string_at(sw - w - 6, sh - 30, ssid,  txt, DESKTOP_BG);
        w = wm_strlen(ipbuf) * 8; draw_string_at(sw - w - 6, sh - 16, ipbuf, txt, DESKTOP_BG);
    } else {
        draw_string_at(sw - 8*9 - 6, sh - 16, "no wifi", txt, DESKTOP_BG);
    }
}

void wm_run(void)
{
    unsigned int frame = 0;
    int sw = (int)video_screen_width();
    int sh = (int)video_screen_height();

    /* The wm now owns the framebuffer; stop the slow text-console mirroring so
     * uart_putc (keyboard echo, command output) stays fast. */
    screen_console_disable();

    /* Render off-screen so the per-frame full-screen wipe + redraw never
     * shows as flicker; each finished frame is flipped to the visible
     * framebuffer in one pass via video_present().  Falls back to direct
     * drawing if the buffer can't be allocated. */
    video_enable_backbuffer();

    /* Initial desktop wipe (screen-space, viewport bypass). */
    video_set_viewport(0, 0);
    fill_rect(0, 0, sw, sh, DESKTOP_BG);

    int s_last_vp_x = -999999, s_last_vp_y = -999999;   /* force a wipe on frame 0 */
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

        /* Repaint the desktop background only when the camera actually moved
         * (auto-pan).  The cursor is no longer composited into the back buffer
         * and the windows repaint their own areas every frame, so when the
         * viewport is static the desktop is unchanged — skipping this full-screen
         * fill each frame roughly halves compose time (faster frames => lower
         * keyboard echo latency + smoother pointer). */
        video_set_viewport(0, 0);
        if (vp_x != s_last_vp_x || vp_y != s_last_vp_y || s_force_wipe) {
            fill_rect(0, 0, sw, sh, DESKTOP_BG);
            s_last_vp_x = vp_x; s_last_vp_y = vp_y; s_force_wipe = 0;
            s_clr_pending = 0;                       /* full wipe subsumes it */
        }

        /* Now switch to the panned camera and draw all windows in
         * virtual desktop coordinates. */
        video_set_viewport(vp_x, vp_y);

        /* Erase just the dragged window's old footprint (virtual coords) before
         * the windows repaint — cheap alternative to a full-screen wipe. */
        if (s_clr_pending) {
            fill_rect(s_clr_x0, s_clr_y0, s_clr_x1 - s_clr_x0, s_clr_y1 - s_clr_y0, DESKTOP_BG);
            s_clr_pending = 0;
        }
        for (window_t *w = wm_head; w; w = w->next) {
            draw_chrome(w);
            if (w->draw_content) w->draw_content(w, frame);
        }

        /* WiFi status icon, screen-space (bottom-right), on top of the windows. */
        video_set_viewport(0, 0);
        draw_wifi_icon(sw, sh);

        /* .avm upload progress bar (screen-space, on top) while a POST
         * /actor/loadvm is streaming in — shows percent + KB transferred. */
        { extern void avm_draw_loadbar(int sw, int sh); avm_draw_loadbar(sw, sh); }

        /* Transient overlay (e.g. the right-click desktop menu) on top of all. */
        if (wm_overlay) wm_overlay();

        /* Cursor is NOT baked into the back buffer (no draw_cursor here): flip
         * the composed frame keeping the live cursor rect intact, then stamp the
         * cursor straight onto HDMI several times before the next slow flip so it
         * tracks the mouse smoothly instead of at the flip rate. */
        (void)draw_cursor; (void)CURSOR_SUBFRAMES; (void)CURSOR_SUBMS;  /* retained */
        video_present_hole();                    /* flip, keeping the live cursor */
        video_cursor_to_front(cursor_x, cursor_y, cursor_visible);  /* re-stamp after flip */
        /* No idle delay: the parallel 720p flip already paces the loop, so run
         * frames back-to-back for the lowest input-echo / drag latency. */
        frame++;
    }
}
