// device/video/shellwin.c — wm-managed shell console.
//
// The kernel previously had two mutually exclusive interaction
// modes: with HDMI plugged in, wm_run() captured the frame loop
// forever and the UART REPL was unreachable; without HDMI it
// fell back to shell_main() on the UART.  This window unifies
// them: every uart_putc() is also captured into a ring of text
// lines, and the wm frame loop polls UART input non-blockingly
// so the shell stays responsive while the rest of the windows
// keep animating.

#include "shellwin.h"
#include "video.h"
#include "uart.h"
#include "shell.h"

window_t shell_win;

/* Scrollback ring: row indices wrap modulo SHELLWIN_ROWS, and
 * `ring_filled` flips once we've written past the bottom and
 * are now overwriting the oldest line. */
static char ring[SHELLWIN_ROWS][SHELLWIN_COLS + 1];
static int  cur_row;
static int  cur_col;
static int  ring_filled;
static int  inited;

/* Pending input line (UART → shell_dispatch_line).  Mirrors the
 * line-editor in uart_getline but stays non-blocking. */
static char inbuf[SHELL_BUFLEN];
static int  inlen;

static void newline(void)
{
    ring[cur_row][cur_col] = 0;
    cur_row = (cur_row + 1) % SHELLWIN_ROWS;
    cur_col = 0;
    ring[cur_row][0] = 0;
    if (cur_row == 0) ring_filled = 1;
}

void shellwin_init(void)
{
    for (int r = 0; r < SHELLWIN_ROWS; r++) ring[r][0] = 0;
    cur_row = 0;
    cur_col = 0;
    ring_filled = 0;
    inlen = 0;
    inited = 1;
}

void shellwin_record_char(char c)
{
    if (!inited) return;
    if (c == '\r') return;        /* uart_puts translates \n → \r\n */
    if (c == '\n') { newline(); return; }
    if (c == 0x08 || c == 0x7F) { /* BS / DEL */
        if (cur_col > 0) {
            cur_col--;
            ring[cur_row][cur_col] = 0;
        }
        return;
    }
    if (c < 0x20) return;          /* drop other control chars */

    if (cur_col >= SHELLWIN_COLS) newline();
    ring[cur_row][cur_col++] = c;
    ring[cur_row][cur_col] = 0;
}

void shellwin_draw(window_t *self, unsigned int frame)
{
    (void)frame;

    int fs = self->font_scale > 0 ? self->font_scale : 1;
    int cx = self->x + 4;
    int cy = self->y + WM_TITLEBAR_H + 4;
    const int line_h = (FONT_HEIGHT + 1) * fs;

    /* Cap visible rows to what physically fits inside the window's
     * content area — the ring may carry more lines than the window
     * can show.  Always display the newest tail of the ring. */
    int content_h = self->height - WM_TITLEBAR_H - 7;
    int max_rows = content_h / line_h;
    if (max_rows < 1) return;
    if (max_rows > SHELLWIN_ROWS) max_rows = SHELLWIN_ROWS;

    int have = ring_filled ? SHELLWIN_ROWS : cur_row + 1;
    int rows = have < max_rows ? have : max_rows;

    /* `start` = oldest row to display.  We want rows ending at cur_row,
     * walking backwards `rows-1` steps modulo SHELLWIN_ROWS. */
    int start = (cur_row - rows + 1 + SHELLWIN_ROWS) % SHELLWIN_ROWS;

    for (int i = 0; i < rows; i++) {
        int r = (start + i) % SHELLWIN_ROWS;
        draw_string_scaled(cx, cy + i * line_h,
                           ring[r], 0xFFCCE0FFU, self->content_bg, fs);
    }
}

void shellwin_handle_key(char c)
{
    if (!inited) return;

    if (c == '\r' || c == '\n') {
        uart_putc('\n');
        if (inlen > 0) {
            inbuf[inlen] = 0;
            shell_dispatch_line(inbuf);
            inlen = 0;
        }
        uart_puts("xinu-pi4$ ");
    } else if (c == 0x08 || c == 0x7F) {
        if (inlen > 0) {
            inlen--;
            uart_putc('\b'); uart_putc(' '); uart_putc('\b');
        }
    } else if (c >= 0x20 && c < 0x7F) {
        if (inlen < (int)sizeof(inbuf) - 1) {
            inbuf[inlen++] = c;
            uart_putc(c);
        }
    }
    /* Other control chars (cursor arrows etc.) — caller is responsible
     * for handling those before reaching us; we drop here. */
}

void shellwin_step(void)
{
    if (!inited) return;

    /* Drain whatever the UART RX FIFO has accumulated since the last
     * frame.  USB keyboard input enters via shellwin_handle_key()
     * from the USPi handler — same code path after the dispatcher. */
    int c;
    while ((c = uart_poll_char()) >= 0) {
        shellwin_handle_key((char)c);
    }
}
