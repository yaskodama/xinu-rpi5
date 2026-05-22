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

    struct window *next;          /* internal — set by wm_add() */
} window_t;

/* Push `w` onto the global window list (it will be redrawn after
 * everything previously added, so later windows are "on top" in
 * draw order, though we don't currently overlap them). */
void wm_add(window_t *w);

/* Main loop.  Clears the desktop to a dark background, walks the
 * window list, and redraws frame by frame at ~20 fps.  Never
 * returns — callers should have finished bootstrapping. */
void wm_run(void);

#endif /* XINU_RPI5_WM_H */
