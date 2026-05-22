// device/video/wm.c — window manager: chrome + redraw loop.

#include "wm.h"
#include "video.h"

#define DESKTOP_BG     0xFF003366U   /* dark navy "desktop"          */
#define DEFAULT_FPS    20            /* 1 frame every 50 ms          */

static window_t *wm_head;

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
    unsigned int sw = video_screen_width();
    unsigned int sh = video_screen_height();

    /* Initial desktop wipe — clears the boot-time UART/banner
     * scribbles that uart_putc() left on the framebuffer. */
    fill_rect(0, 0, (int)sw, (int)sh, DESKTOP_BG);

    for (;;) {
        for (window_t *w = wm_head; w; w = w->next) {
            draw_chrome(w);
            if (w->draw_content) w->draw_content(w, frame);
        }
        delay_ms(1000 / DEFAULT_FPS);
        frame++;
    }
}
