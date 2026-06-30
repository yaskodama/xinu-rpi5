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

/* Up to BASICWIN_N on-screen BASIC windows, each with a FULLY INDEPENDENT
 * interpreter instance (program / variables / text buffer).  Window 0 is the
 * primary `basic_win` wired at boot; the right-click desktop menu's "BASIC"
 * entry builds the rest on demand via basicwin_new().  Must be <= NBASIC in
 * basic.c. */
#define BASICWIN_N 4

extern window_t basic_win;

void basicwin_init(void);                                /* set seams + clear   */
void basicwin_draw(window_t *self, unsigned int frame);  /* wm draw_content     */
void basicwin_handle_key(char c);                        /* keyboard -> primary REPL */
void basicwin_new(void);                                 /* spawn another BASIC window */
int  basicwin_route_key(window_t *aw, char c);           /* key -> focused BASIC window */
int  basicwin_is_basic(window_t *w);                     /* true for any BASIC window */

/* Remote control (HTTP test harness, system/tcp_server.c). */
void basicwin_post_line(const char *s);   /* queue a direct command (run from tick) */
void basicwin_poll_pending(void);         /* run a queued command — call from wm tick */
void basicwin_inject_key(char c);         /* feed a key as if typed (Ctrl-C breaks) */
int  basicwin_running(void);              /* 1 while a program executes */

#endif /* XINU_RPI5_BASICWIN_H */
