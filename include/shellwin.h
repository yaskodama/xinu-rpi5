// include/shellwin.h — Shell-as-a-window.
//
// A scroll-back console rendered into one wm window.  Every byte
// that flows through uart_putc() is also captured into a fixed
// ring of text lines so the shell stays readable on HDMI even
// after the wm has repainted the desktop.
//
// Input is still UART-only for now (USB HID will land in phase
// M5).  shellwin_step() polls the PL011 RX FIFO non-blockingly,
// builds up a line, and dispatches via shell_dispatch_line() once
// a CR/LF arrives — so the wm_run() frame loop can drive the
// shell without giving up the CPU to a blocking uart_getline().

#ifndef XINU_RPI5_SHELLWIN_H
#define XINU_RPI5_SHELLWIN_H

#include "wm.h"

/* Visible scrollback size in the shell window.  18 rows × 72 cols
 * fits cleanly under our 640×480 layout with the 8×8 font.  Each
 * row stores a NUL-terminated string. */
#define SHELLWIN_ROWS    112   /* enough rows to fill the tall shell window at 1080p */
#define SHELLWIN_COLS    92

/* Clear the ring and reset write head.  Must be called before any
 * uart traffic if shellwin output is to be captured. */
void shellwin_init(void);

/* Append one character to the ring.  '\n' advances to the next
 * row (carriage return is ignored — uart_puts() translates).
 * Backspace / DEL erase the previous column on the current row.
 * Other control chars are dropped.  Safe before shellwin_init()
 * (no-op) so wiring it into uart_putc() can't crash early boot. */
void shellwin_record_char(char c);

/* draw_content callback for wm — paints the visible ring onto
 * self's content area in chronological order. */
void shellwin_draw(window_t *self, unsigned int frame);

/* Drive one non-blocking shell step.  Drains the UART RX FIFO,
 * accumulates an input line, dispatches via shell_dispatch_line()
 * on CR/LF.  Designed to be called from the wm_run() frame loop. */
void shellwin_step(void);

/* Feed one keypress into the shell input buffer.  Same line-editor
 * behaviour as the UART path (CR/LF → dispatch, BS/DEL → backspace,
 * printable → echo + buffer).  Used by the USPi keyboard handler
 * so USB keystrokes share the UART input path. */
void shellwin_handle_key(char c);

/* The window descriptor itself — laid out / wm_add()'d by
 * loader/main.c.  draw_content already points at shellwin_draw. */
extern window_t shell_win;

#endif /* XINU_RPI5_SHELLWIN_H */
