// include/softkbd.h — on-screen soft keyboard window.
//
// Renders a QWERTY keyboard inside one wm window.  Input dispatch
// happens later (UI-K1) once a mouse cursor exists to click with;
// for now the window is purely visual.
//
// Layout: five rows × variable columns.  Each key is a rounded
// rectangle with a centred glyph.  Modifier state (caps/shift) is
// tracked internally so the same rendering routine can paint the
// uppercase variant later.

#ifndef XINU_RPI5_SOFTKBD_H
#define XINU_RPI5_SOFTKBD_H

#include "wm.h"

void softkbd_draw(window_t *self, unsigned int frame);

/* Hit-test a click at virtual-desktop point (sx,sy) against the soft-keyboard
 * key grid and return the character it produces (ASCII, plus 0x08 Bksp / '\t' /
 * '\r'), or 0 if the point hit no key or a pure modifier (Shift/Caps/Ctrl/Alt,
 * whose state is updated internally).  Caller routes the char to the focused
 * window — see xhci_mouse_event() in loader/main.c. */
char softkbd_hit(int sx, int sy);

extern window_t softkbd_win;

#endif /* XINU_RPI5_SOFTKBD_H */
