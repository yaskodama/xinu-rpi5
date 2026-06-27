// include/video.h — Pi 5 HDMI framebuffer console.
//
// Asks the firmware (via the VC mailbox property channel) for a
// 32-bpp framebuffer at a fixed resolution, then renders text into
// it using an embedded 8x8 ASCII font.  The shell `uart_putc` is
// wired to call screen_putc() as well, so output appears on HDMI in
// parallel with the (possibly silent) UART.
//
// Designed to fail gracefully: if the mailbox call times out (e.g.
// running under QEMU virt, which has no VC mailbox), screen_ready()
// stays false and screen_putc() becomes a no-op.

#ifndef XINU_RPI5_VIDEO_H
#define XINU_RPI5_VIDEO_H

/* HDMI scanout mode 1920x1080 — full screen.  The window manager's virtual
 * desktop (WM_DESKTOP_W x WM_DESKTOP_H in wm.h) is the same 1920x1080, so the
 * viewport never has anywhere to pan: the whole desktop is on screen at once
 * (no scrolling).  Overridable from the Makefile. */
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH    1920
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT   1080
#endif
#define SCREEN_DEPTH    32
#define FONT_WIDTH      8
#define FONT_HEIGHT     8

/* Attempt to bring up the framebuffer.  Returns 0 on success.
 * Caller can ignore failure and continue with UART-only logging. */
int  video_init(void);

/* True iff video_init() succeeded and screen_putc is live. */
int  screen_ready(void);

/* Push one character to the screen console.  Honours '\n' (new line
 * + carriage return) and '\r' (column 0).  Wraps at right edge,
 * scrolls one row at the bottom. */
void screen_putc(char c);
/* Stop mirroring uart output to the (slow, now-hidden) text console once the wm
 * owns the framebuffer — keeps typing snappy and big command output non-blocking. */
void screen_console_disable(void);

void screen_puts(const char *s);

/* Glyph table — declared here so the font file is the only place
 * that owns the bitmap data.  96 printable chars (ASCII 0x20..0x7F),
 * 8 bytes each (one per row, MSB = leftmost pixel). */
extern const unsigned char font8x8[96][8];

/* ===== Drawing primitives (implemented in video.c) ============== */

/* Live FB dimensions — only meaningful after a successful
 * video_init().  Used by wm.c to size the desktop. */
unsigned int video_screen_width(void);
unsigned int video_screen_height(void);

/* Double buffering (anti-flicker).  video_enable_backbuffer() allocates
 * an off-screen render target the size of the framebuffer and points all
 * drawing primitives at it (returns 1 on success, 0 if no memory — in
 * which case drawing stays direct).  video_present() copies the finished
 * off-screen frame to the visible framebuffer in one pass.  Call
 * enable once, then present() at the end of every rendered frame. */
int  video_enable_backbuffer(void);
void video_present(void);
/* Flip everything except the live cursor rect (so a directly-stamped cursor
 * survives the flip), and stamp the decoupled cursor straight onto HDMI. */
void video_present_hole(void);
void video_cursor_to_front(int x, int y, int visible);

/* Viewport (camera) on a larger virtual desktop.  All drawing
 * primitives below (fill_rect / draw_rect / draw_string_at /
 * draw_glyph_at) accept *virtual* desktop coordinates and apply
 * this offset + clip-to-physical-screen before writing pixels.
 *
 * The default offset is (0, 0) — when nothing scrolls, virtual
 * coords map 1:1 to screen coords.  wm.c owns the panning logic;
 * direct callers (e.g. wm's cursor overlay) reset the offset to
 * (0, 0) briefly when they need screen-space drawing. */
void video_set_viewport(int x, int y);
int  video_viewport_x(void);
int  video_viewport_y(void);

/* Solid fill / outline rectangle at pixel (x,y), dimensions w×h. */
void fill_rect(int x, int y, int w, int h, unsigned int color);
void draw_rect(int x, int y, int w, int h, unsigned int color);
void draw_line(int x0, int y0, int x1, int y1, unsigned int color);

/* 8x8 glyph drawing with explicit foreground / background colour.
 * draw_string_at advances 8 pixels per character; caller wraps. */
void draw_glyph_at(int px, int py, char c,
                   unsigned int fg, unsigned int bg);
void draw_string_at(int px, int py, const char *s,
                    unsigned int fg, unsigned int bg);

/* Magnified glyph / string (scale 1 == the 8x8 default).  Used by the
 * BASIC window's toolbar / text grid. */
void draw_glyph_scaled(int px, int py, char c,
                       unsigned int fg, unsigned int bg, int scale);
void draw_string_scaled(int px, int py, const char *s,
                        unsigned int fg, unsigned int bg, int scale);

/* BASIC graphics canvas (LINE / CIRCLE / PLOT display list).  Coords are
 * window-content-relative; bgfx_render() offsets them into the content rect
 * each frame and clips to it.  Colour args are BASIC palette indices (& 7). */
void bgfx_clear(void);
void bgfx_line(int x0, int y0, int x1, int y1, int color);
void bgfx_circle(int cx, int cy, int r, int color);
void bgfx_render(int ox, int oy, int w, int h);

/* Busy-wait based on the AArch64 generic timer (CNTPCT_EL0).
 * Used for animation pacing in wm_run(). */
void delay_ms(unsigned int ms);

#endif /* XINU_RPI5_VIDEO_H */
