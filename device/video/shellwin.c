// device/video/shellwin.c — wm-managed shell console(s).
//
// The kernel previously had two mutually exclusive interaction
// modes: with HDMI plugged in, wm_run() captured the frame loop
// forever and the UART REPL was unreachable; without HDMI it
// fell back to shell_main() on the UART.  This window unifies
// them: every uart_putc() is also captured into a ring of text
// lines, and the wm frame loop polls UART input non-blockingly
// so the shell stays responsive while the rest of the windows
// keep animating.
//
// Multi-instance: up to SHELLWIN_MAX shell windows, each with its own
// scrollback ring + input line.  Exactly one is "active" at a time — it
// receives the captured uart_putc() output and keyboard input.  Focusing a
// shell window (mouse) makes it active (see loader/main.c).  Additional shell
// windows are spawned from the right-click desktop menu via shellwin_spawn().

#include "shellwin.h"
#include "video.h"
#include "uart.h"
#include "shell.h"

#define SHELLWIN_MAX 4

typedef struct {
    char ring[SHELLWIN_ROWS][SHELLWIN_COLS + 1];
    int  cur_row, cur_col, ring_filled;
    char inbuf[SHELL_BUFLEN];
    int  inlen;
} shell_inst_t;

static shell_inst_t insts[SHELLWIN_MAX];
static int          n_inst;          /* number of live shells (>=1 after init) */
static int          active;          /* instance receiving uart capture + keys */
static int          inited;

/* The first shell's window (instance 0) lives in shellwin.c; extra shells use
 * these pre-allocated window structs (no heap alloc from the input path). */
window_t shell_win;
static window_t shell_extra[SHELLWIN_MAX - 1];

static void inst_newline(shell_inst_t *in)
{
    in->ring[in->cur_row][in->cur_col] = 0;
    in->cur_row = (in->cur_row + 1) % SHELLWIN_ROWS;
    in->cur_col = 0;
    in->ring[in->cur_row][0] = 0;
    if (in->cur_row == 0) in->ring_filled = 1;
}

static void inst_reset(shell_inst_t *in)
{
    for (int r = 0; r < SHELLWIN_ROWS; r++) in->ring[r][0] = 0;
    in->cur_row = 0;
    in->cur_col = 0;
    in->ring_filled = 0;
    in->inlen = 0;
}

void shellwin_init(void)
{
    inst_reset(&insts[0]);
    n_inst = 1;
    active = 0;
    shell_win.tag = 0;
    inited = 1;

    /* Show a live prompt immediately so the window reads as a running shell.
     * uart_puts() mirrors into the active scrollback ring (see uart_putc). */
    uart_puts("Embedded Xinu (Pi 5) shell -- type 'help'\nxinu-pi5$ ");
}

void shellwin_clear(void)
{
    if (!inited) return;
    inst_reset(&insts[active]);
}

void shellwin_record_char(char c)
{
    if (!inited) return;
    shell_inst_t *in = &insts[active];
    if (c == '\r') return;        /* uart_puts translates \n → \r\n */
    if (c == '\n') { inst_newline(in); return; }
    if (c == 0x08 || c == 0x7F) { /* BS / DEL */
        if (in->cur_col > 0) {
            in->cur_col--;
            in->ring[in->cur_row][in->cur_col] = 0;
        }
        return;
    }
    if (c < 0x20) return;          /* drop other control chars */

    if (in->cur_col >= SHELLWIN_COLS) inst_newline(in);
    in->ring[in->cur_row][in->cur_col++] = c;
    in->ring[in->cur_row][in->cur_col] = 0;
}

void shellwin_draw(window_t *self, unsigned int frame)
{
    int idx = self->tag;
    if (idx < 0 || idx >= SHELLWIN_MAX) idx = 0;
    shell_inst_t *in = &insts[idx];

    int cx = self->x + 4;
    int cy = self->y + WM_TITLEBAR_H + 4;
    const int line_h = FONT_HEIGHT + 1;

    /* Cap visible rows to what physically fits inside the window's
     * content area — the ring may carry more lines than the window
     * can show.  Always display the newest tail of the ring. */
    int content_h = self->height - WM_TITLEBAR_H - 7;
    int max_rows = content_h / line_h;
    if (max_rows < 1) return;
    if (max_rows > SHELLWIN_ROWS) max_rows = SHELLWIN_ROWS;

    int have = in->ring_filled ? SHELLWIN_ROWS : in->cur_row + 1;
    int rows = have < max_rows ? have : max_rows;

    /* `start` = oldest row to display.  We want rows ending at cur_row,
     * walking backwards `rows-1` steps modulo SHELLWIN_ROWS. */
    int start = (in->cur_row - rows + 1 + SHELLWIN_ROWS) % SHELLWIN_ROWS;

    for (int i = 0; i < rows; i++) {
        int r = (start + i) % SHELLWIN_ROWS;
        draw_string_at(cx, cy + i * line_h,
                       in->ring[r], 0xFFCCE0FFU, self->content_bg);
    }

    /* Input caret at the current write position (end of the prompt line, which
     * is always the newest displayed row).  Only the active shell blinks a
     * bright caret; inactive shells show a dim one. */
    {
        int caret_x = cx + in->cur_col * FONT_WIDTH;
        int caret_y = cy + (rows - 1) * line_h;
        unsigned int col;
        if (idx == active) col = ((frame >> 4) & 1) ? 0xFF00FF00U : 0xFF006600U;
        else               col = 0xFF004400U;
        fill_rect(caret_x, caret_y, FONT_WIDTH, FONT_HEIGHT, col);
    }
}

void shellwin_handle_key(char c)
{
    if (!inited) return;
    shell_inst_t *in = &insts[active];

    if (c == '\r' || c == '\n') {
        uart_putc('\n');
        if (in->inlen > 0) {
            in->inbuf[in->inlen] = 0;
            shell_dispatch_line(in->inbuf);
            in->inlen = 0;
        }
        uart_puts("xinu-pi5$ ");
    } else if (c == 0x08 || c == 0x7F) {
        if (in->inlen > 0) {
            in->inlen--;
            uart_putc('\b'); uart_putc(' '); uart_putc('\b');
        }
    } else if (c >= 0x20 && c < 0x7F) {
        if (in->inlen < (int)sizeof(in->inbuf) - 1) {
            in->inbuf[in->inlen++] = c;
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

/* True if `w` is one of the shell-console windows. */
int shellwin_is_shell(window_t *w)
{
    return w && w->draw_content == shellwin_draw;
}

/* Make the shell bound to window `w` the active one (gets uart capture + keys).
 * No-op if `w` is not a shell window. */
void shellwin_set_active(window_t *w)
{
    if (!shellwin_is_shell(w)) return;
    int idx = w->tag;
    if (idx >= 0 && idx < n_inst) active = idx;
}

/* Open an additional shell window (right-click desktop menu → "Shell").  Runs
 * in the wm main loop (mouse handler), so wm_add() is safe here.  Caps out at
 * SHELLWIN_MAX shells. */
void shellwin_spawn(void)
{
    extern int  wm_view_x(void);
    extern int  wm_view_y(void);

    if (!inited || n_inst >= SHELLWIN_MAX) return;
    int idx = n_inst++;
    inst_reset(&insts[idx]);

    window_t *w = &shell_extra[idx - 1];
    int vx = wm_view_x(), vy = wm_view_y();
    /* Cascade the new window into the visible viewport. */
    w->x = vx + 120 + idx * 36;
    w->y = vy + 90  + idx * 36;
    w->width  = 760;
    w->height = 440;
    const char *t = "Shell";
    int i = 0;
    for (; i < WM_TITLE_MAX && t[i]; i++) w->title[i] = t[i];
    w->title[i] = 0;
    w->chrome_color = 0xFF80E080U;
    w->title_bg     = 0xFF205020U;
    w->title_fg     = 0xFFFFFFFFU;
    w->content_bg   = 0xFF000010U;
    w->draw_content = shellwin_draw;
    w->tag          = idx;
    w->focused      = 0;
    w->next         = 0;

    wm_add(w);                       /* link into the window list (drawn on top) */

    /* Make the new shell the focused + active one and greet it.  wm_focus_at
     * takes screen coords (it re-adds the viewport), so map the new window's
     * title-bar centre back to screen space. */
    extern window_t *wm_focus_at(int, int);
    wm_focus_at((w->x - vx) + w->width / 2, (w->y - vy) + 6);
    active = idx;
    uart_puts("Embedded Xinu (Pi 5) shell -- type 'help'\nxinu-pi5$ ");
}
