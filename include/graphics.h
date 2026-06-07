// include/graphics.h — 3D wireframe wine-glass window.
#ifndef XINU_RPI5_GRAPHICS_H
#define XINU_RPI5_GRAPHICS_H

#include "wm.h"

extern window_t graphics_win;

/* draw_content callback: renders the rotating wine glass. */
void graphics_draw(window_t *self, unsigned int frame);

/* Start the 30-step rotation about x/y/z (called by the `wine` shell command). */
void graphics_wine_start(void);

/* Start the 30-step four-rotating-lines animation (the `4lines` command). */
void graphics_4lines_start(void);

#endif /* XINU_RPI5_GRAPHICS_H */
