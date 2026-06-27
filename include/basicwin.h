// include/basicwin.h — wm-managed BASIC interpreter window (Pi 5).
//
// A self-contained on-screen BASIC: the interpreter core lives in
// device/video/basic.c (freestanding, driven through callback seams);
// this window provides the text grid, the line editor (basicwin_handle_key)
// and the output sink (bw_emit), and drives the interpreter one typed line
// at a time via basic_exec_line().  A toolbar below the title bar injects
// direct commands (FILES / LIST / RUN "<sample>") so the embedded samples
// run with one click — no filesystem needed.
//
// Ported from the rpi4 BASIC window, trimmed to a single on-screen instance
// to match the rpi5 desktop (the wm here has no on-demand window spawning for
// BASIC).

#ifndef XINU_RPI5_BASICWIN_H
#define XINU_RPI5_BASICWIN_H

#include "wm.h"

extern window_t basic_win;

void basicwin_init(void);                                /* set seams + clear   */
void basicwin_draw(window_t *self, unsigned int frame);  /* wm draw_content     */
void basicwin_handle_key(char c);                        /* keyboard -> REPL    */
int  basicwin_is_basic(window_t *w);                     /* true for basic_win  */

#endif /* XINU_RPI5_BASICWIN_H */
