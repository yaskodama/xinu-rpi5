// include/graphics.h — 3D wireframe wine-glass window.
#ifndef XINU_RPI5_GRAPHICS_H
#define XINU_RPI5_GRAPHICS_H

#include "wm.h"

extern window_t graphics_win;

/* draw_content callback: renders the rotating wine glass. */
void graphics_draw(window_t *self, unsigned int frame);

/* Start the 30-step rotation about x/y/z (called by the `wine` shell command). */
void graphics_wine_start(void);

/* Start the four-rotating-lines animation (the `4lines` command). */
void graphics_4lines_start(void);

/* Start the rotating 3D "コダマ" block text (the `kodama` command). */
void graphics_kodama_start(void);

/* Start the four-segments-on-a-square animation for `turns` revolutions (same
 * as `4lines` but with a chosen turn count).  Driven by the AIPL sample
 * RotateLine.abcl through the JIT builtin gfx_rotate_line(). */
void graphics_rotateline_start(int turns);

#endif /* XINU_RPI5_GRAPHICS_H */
