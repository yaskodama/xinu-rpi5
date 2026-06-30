// include/wm.h — minimal window manager for the HDMI framebuffer.
//
// A window is a fixed-position, fixed-size rectangle with a title
// bar at the top and a content area below.  Multiple windows are
// registered up-front via wm_add(); after that wm_run() takes over
// and repaints them all every ~20 fps, calling each window's
// draw_content() callback with a monotonically-increasing frame
// counter so callbacks can animate / show live data.
//
// No input plumbing yet — there is no USB stack so neither USB
// keyboards nor mice reach us.  Windows are static layout; what
// makes them interesting is what their content callbacks do
// (clocks, animations, scheduler telemetry).

#ifndef XINU_RPI5_WM_H
#define XINU_RPI5_WM_H

#define WM_TITLE_MAX  31
#define WM_TITLEBAR_H 12          /* 8 px glyph + 2 px padding top/bottom */

typedef struct window {
    int           x, y;
    int           width, height;
    char          title[WM_TITLE_MAX + 1];

    /* ARGB colours for the window chrome and content background. */
    unsigned int  chrome_color;
    unsigned int  title_bg;
    unsigned int  title_fg;
    unsigned int  content_bg;

    /* Called every frame after the chrome is repainted.  The
     * implementation may draw anywhere inside the content area
     * (x+1 .. x+width-2  ×  y+TITLEBAR_H+1 .. y+height-2). */
    void        (*draw_content)(struct window *self, unsigned int frame);

    /* Optional: called once on the left-button PRESS edge when the click lands
     * inside this window's body (below the title bar).  (lx,ly) are window-local
     * pixel coords.  Used by the BASIC window for its toolbar buttons.  NULL =
     * no per-window click handling. */
    void        (*on_click)(struct window *self, int lx, int ly);

    int           focused;        /* 1 = currently selected window (bright chrome) */
    int           font_scale;     /* content text magnification (0/1 = 1x, 2 = 2x …);
                                   * windows that honour it draw via draw_string_scaled */
    int           tag;            /* free for the owner — shellwin uses it as the
                                   * shell-instance index (0 = the first shell). */
    struct window *next;          /* internal — set by wm_add() */
} window_t;

/* Hit-test a screen-space point (e.g. the mouse cursor) against the window
 * list and focus + raise the topmost window under it.  No-op if the point
 * misses every window.  Returns the focused window, or NULL. */
struct window *wm_focus_at(int screen_x, int screen_y);

/* The window currently focused via wm_focus_at(), or NULL. */
struct window *wm_focused(void);

/* Topmost window under a screen-space point, without focusing/raising it. */
struct window *wm_window_at(int screen_x, int screen_y);

/* True if virtual-desktop point (vx,vy) hits `w`'s title-bar close button. */
int wm_close_hit(struct window *w, int vx, int vy);

/* True if `w` is currently in the window list (shown / interactive). */
int wm_is_shown(struct window *w);

/* Close (remove) `w` from the window list; focus passes to the new topmost. */
void wm_remove(struct window *w);

/* Force a full desktop repaint on the next frame (call after moving a window so
 * its old position doesn't leave a trail now that the per-frame wipe is skipped). */
void wm_request_full_redraw(void);

/* Queue a background-clear of one virtual-desktop rectangle for the next frame
 * (unions with any already queued).  Cheaper than wm_request_full_redraw() —
 * used while dragging/resizing to erase a window's old footprint without
 * wiping the whole screen, keeping the drag smooth. */
void wm_clear_bg_rect(int x, int y, int w, int h);

/* Scroll the viewport over the (larger) virtual desktop by (dx,dy) pixels.
 * Clamped to the desktop bounds each frame.  Driven by the mouse edge-pan. */
void wm_scroll(int dx, int dy);

/* Push `w` onto the global window list (it will be redrawn after
 * everything previously added, so later windows are "on top" in
 * draw order, though we don't currently overlap them). */
void wm_add(window_t *w);

/* Add `w` to the desktop if not already shown, then focus + raise it.  The
 * Pi 4 wm_show() equivalent — used by the right-click menu to pop up an
 * on-demand window (shell / BASIC / AVM list). */
void wm_show(window_t *w);

/* Reposition / resize a window by its add order (0-based).  Used by the
 * serial layout-command parser so the Mac screen-designer can rearrange the
 * live desktop over the debug UART (Pi 5 has no network). */
struct window *wm_nth(int id);
void wm_move_window(int id, int x, int y);
void wm_resize_window(int id, int w, int h);

/* Main loop.  Clears the desktop to a dark background, walks the
 * window list, and redraws frame by frame at ~20 fps.  Never
 * returns — callers should have finished bootstrapping. */
void wm_run(void);

/* Register a function to be called at the top of every wm_run
 * iteration, before window repainting.  Used by shellwin to drive
 * the non-blocking REPL on each frame.  Pass NULL to detach. */
void wm_set_tick(void (*fn)(void));

/* Register a function called once per frame AFTER every window (and the WiFi
 * icon) is drawn but BEFORE the frame is flipped — i.e. it paints on top of
 * everything.  Used for transient overlays like the right-click desktop menu.
 * Pass NULL to detach. */
void wm_set_overlay(void (*fn)(void));

/* Move / hide the on-screen mouse cursor overlay.  Painted by
 * wm_run() after all windows so it stays on top.  Visible=0 hides
 * the cursor entirely.  Caller chooses screen-space pixel coords. */
void wm_cursor_set(int x, int y, int visible);

/* Virtual desktop is WM_DESKTOP_W × WM_DESKTOP_H.  Viewport (the
 * camera onto the desktop) is the size of the physical screen.
 * wm_pan(dx, dy) shifts the viewport by (dx, dy) pixels, clamping
 * so the viewport never leaves the desktop.  wm_set_viewport()
 * is the absolute version. */
#define WM_DESKTOP_W   1920
#define WM_DESKTOP_H   1080

void wm_pan(int dx, int dy);
void wm_set_viewport(int x, int y);
int  wm_view_x(void);
int  wm_view_y(void);

/* Toggle the slow auto-pan demo (used until USB keyboard / mouse
 * scroll is wired).  On by default at boot so the user can see
 * the larger desktop scroll without any input.  Pass 0 to stop. */
void wm_set_autopan(int on);

#endif /* XINU_RPI5_WM_H */
