// device/video/basicwin.c — wm-managed BASIC window as a full-screen text
// editor (Pi 5, single instance).
//
// No prompt — just a block cursor in a large space-padded text grid (~10
// screens of scrollback).  Arrow keys move the cursor freely; typing inserts
// at the cursor; Enter runs the cursor's line through basic_exec_line() and the
// interpreter's output (bw_emit) flows from the cursor like a terminal.  A
// clickable toolbar below the title bar injects FILES / LIST / RUN "<sample>"
// so the embedded samples run with one click.  BASIC's LINE/CIRCLE/PLOT draw
// into a graphics overlay (bgfx_* display list in video.c).
//
// Ported from rpi4 device/video/basicwin.c, flattened to one interpreter
// instance (instance 0) and adapted to the rpi5 window_t / video API.

#include "basicwin.h"
#include "video.h"

/* ---- interpreter seams (device/video/basic.c) ---- */
extern void basic_init(void);
extern void basic_exec_line(const char *line);
extern void basic_set_emit(void (*fn)(const char *));
extern void basic_set_cls(void (*fn)(int));
extern void basic_set_pause(void (*fn)(int));
extern void basic_set_break_poll(int (*fn)(void));
extern void basic_set_line(void (*fn)(int, int, int, int, int));
extern void basic_set_circle(void (*fn)(int, int, int, int));
extern void basic_set_plot(void (*fn)(int, int, int));
extern void basic_set_gfx_active(int (*fn)(void));
extern void basic_set_button(void (*fn)(int, const char *));
extern void basic_set_btn(int (*fn)(int));
extern void basic_set_buttons_reset(void (*fn)(void));
extern void basic_select(int inst);        /* pick which bs[] instance is current */
extern int  basic_break_pending(void);     /* 1 if a Ctrl-C break is queued */
extern void video_present(void);           /* flip back buffer -> HDMI */

#define BW_COLS   96            /* chars per row (fixed grid)              */
#define BW_ROWS   360           /* ~10 screens of scrollback              */

window_t basic_win;

/* ---- text grid state (single instance) ---- */
static char grid[BW_ROWS][BW_COLS + 1];   /* space-padded; trailing NUL for drawing */
static int  cur_row, cur_col;             /* cursor in buffer coords                */
static int  view_top;                     /* first buffer row shown                 */
static int  inited;
static int  esc_state;                    /* ANSI escape parser: 0 none,1 ESC,2 ESC[ */
static int  gfx_on;                       /* 1 once a program enters graphics mode   */
static int  gfx_dirty;                    /* 1 if the gfx list changed since last flip */

static int bw_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* ---- toolbar buttons -------------------------------------------------- */
typedef struct { const char *label; const char *cmd; } bw_button_t;
static const bw_button_t bw_btns[] = {
    { "FILES", "FILES"          },
    { "LIST",  "LIST"           },
    { "hanoi", "RUN \"hanoi\""  },
    { "bsort", "RUN \"bsort\""  },
    { "fizz",  "RUN \"fizz\""   },
    { "qsort", "RUN \"qsort\""  },
    { "koch",  "RUN \"koch\""   },
    { "maze",  "RUN \"maze\""   },
    { "glass", "RUN \"glass\""  },
    { "rescue","RUN \"rescue\"" },
};
#define BW_NBTN      ((int)(sizeof(bw_btns) / sizeof(bw_btns[0])))
#define BW_BTN_H     16
#define BW_BTN_GAP   2
#define BW_TOOLBAR_H (BW_BTN_H + 4)     /* button strip height below titlebar */

/* Button i rectangle in WINDOW-LOCAL coords (relative to self->x/self->y). */
static void bw_btn_rect(int i, int win_w, int *bx, int *by, int *bw, int *bh)
{
    int avail = win_w - 4 - BW_BTN_GAP * (BW_NBTN - 1);
    int w = avail / BW_NBTN;
    if (w < 1) w = 1;
    *bw = w; *bh = BW_BTN_H;
    *bx = 2 + i * (w + BW_BTN_GAP);
    *by = WM_TITLEBAR_H + 2;
}

/* ---- program buttons: BASIC `BUTTON n,"label"[,line]` ----------------- */
#define PBTN_MAX 8
#define BW_PBTN_H 15
static char pbtn_label[PBTN_MAX][24];
static int  pbtn_present[PBTN_MAX];
static int  pbtn_clicks[PBTN_MAX];

static int pbtn_count(void) { int c = 0; for (int i = 0; i < PBTN_MAX; i++) if (pbtn_present[i]) c++; return c; }

static int pbtn_rect(int win_w, int n, int *bx, int *by, int *bw, int *bh)
{
    if (n < 0 || n >= PBTN_MAX || !pbtn_present[n]) return 0;
    int cnt = pbtn_count(); if (cnt < 1) return 0;
    int idx = 0; for (int i = 0; i < n; i++) if (pbtn_present[i]) idx++;
    int avail = win_w - 4 - BW_BTN_GAP * (cnt - 1);
    int w = avail / cnt; if (w < 1) w = 1;
    *bw = w; *bh = BW_PBTN_H;
    *bx = 2 + idx * (w + BW_BTN_GAP);
    *by = WM_TITLEBAR_H + BW_TOOLBAR_H + 1;
    return 1;
}
static void bw_button(int n, const char *label)
{
    if (n < 0 || n >= PBTN_MAX) return;
    int i = 0; for (; label[i] && i < 23; i++) pbtn_label[n][i] = label[i];
    pbtn_label[n][i] = 0; pbtn_present[n] = 1;
}
static int  bw_btn(int n) { if (n < 0 || n >= PBTN_MAX) return 0; int c = pbtn_clicks[n]; pbtn_clicks[n] = 0; return c; }
static void bw_buttons_reset(void) { for (int i = 0; i < PBTN_MAX; i++) { pbtn_present[i] = 0; pbtn_clicks[i] = 0; pbtn_label[i][0] = 0; } }

static void bw_draw_pbtns(window_t *self)
{
    for (int n = 0; n < PBTN_MAX; n++) {
        int bx, by, bw, bh;
        if (!pbtn_rect(self->width, n, &bx, &by, &bw, &bh)) continue;
        int ax = self->x + bx, ay = self->y + by;
        unsigned int bg = 0xFF9A5A12U;                    /* amber program button */
        fill_rect(ax, ay, bw, bh, bg);
        draw_rect(ax, ay, bw, bh, 0xFFFFC766U);
        int lw = bw_strlen(pbtn_label[n]) * FONT_WIDTH;
        int tx = ax + (bw - lw) / 2; if (tx < ax + 1) tx = ax + 1;
        int ty = ay + (bh - FONT_HEIGHT) / 2;
        draw_string_at(tx, ty, pbtn_label[n], 0xFFFFFFFFU, bg);
    }
}

/* Graphics canvas rect = the content area below the toolbar. */
static void bw_gfx_rect(window_t *self, int *gx, int *gy, int *gw, int *gh)
{
    *gx = self->x + 1;
    *gy = self->y + WM_TITLEBAR_H + BW_TOOLBAR_H + 2;
    *gw = self->width - 2;
    *gh = self->height - WM_TITLEBAR_H - BW_TOOLBAR_H - 3;
}

static void clear_row(int r) { for (int i = 0; i < BW_COLS; i++) grid[r][i] = ' '; grid[r][BW_COLS] = 0; }
static void clear_all(void)  { for (int r = 0; r < BW_ROWS; r++) clear_row(r); cur_row = cur_col = view_top = 0; }

static void scroll_buffer(void)
{
    for (int r = 0; r < BW_ROWS - 1; r++)
        for (int i = 0; i <= BW_COLS; i++) grid[r][i] = grid[r + 1][i];
    clear_row(BW_ROWS - 1);
    if (cur_row > 0) cur_row--;
    if (view_top > 0) view_top--;
}

static void cursor_down_one(void)
{
    cur_row++;
    if (cur_row >= BW_ROWS) { scroll_buffer(); cur_row = BW_ROWS - 1; }
}

/* ---- interpreter output sink: terminal-style write at the cursor ---- */
static void bw_putc(char c)
{
    if (c == '\r') { cur_col = 0; return; }
    if (c == '\n') { cur_col = 0; cursor_down_one(); return; }
    if (c == '\b') { if (cur_col > 0) cur_col--; return; }
    if (c < 0x20 || c > 0x7e) return;
    if (cur_col >= BW_COLS) { cur_col = 0; cursor_down_one(); }
    grid[cur_row][cur_col++] = c;
}
static void bw_emit(const char *s) { while (*s) bw_putc(*s++); }

/* CLS:  bit 1 = text grid, bit 2 = graphics. */
static void bw_cls(int mode)
{
    if (mode & 1) clear_all();
    if (mode & 2) { bgfx_clear(); gfx_on = 1; gfx_dirty = 1; }
    if (mode == 0) clear_all();          /* bare CLS == text clear */
}

/* Repaint the BASIC graphics area cleanly and flip it to screen — lets the
 * single-threaded interpreter show animation between PAUSEs (the wm render loop
 * is blocked while a program RUNs). */
static void bw_present_gfx(void)
{
    int gx, gy, gw, gh;
    bw_gfx_rect(&basic_win, &gx, &gy, &gw, &gh);
    fill_rect(gx, gy, gw, gh, basic_win.content_bg);
    bgfx_render(gx, gy, gw, gh);
    bw_draw_pbtns(&basic_win);      /* keep program buttons painted during a RUN */
    video_present();
}

/* No Ctrl-C break wired on rpi5 (no xhci ctrl-c poll seam) — programs run to
 * completion.  Returning 0 means "no break requested". */
static int bw_break_poll(void) { return 0; }

static void bw_pause(int ms)
{
    if (gfx_on && gfx_dirty) { bw_present_gfx(); gfx_dirty = 0; }
    /* Sleep in small slices so a Ctrl-C (set from the keyboard interrupt while
     * the RUN loop is blocked here) aborts the PAUSE promptly instead of after
     * the full delay. */
    while (ms > 0) {
        if (basic_break_pending()) return;
        int slice = ms < 20 ? ms : 20;
        delay_ms((unsigned int)slice);
        ms -= slice;
    }
}

/* ---- graphics seams: BASIC LINE/CIRCLE/PLOT -> bgfx_* display list. ---- */
static void bw_line(int x1, int y1, int x2, int y2, int color)
{ gfx_on = 1; gfx_dirty = 1; bgfx_line(x1, y1, x2, y2, color); }
static void bw_circle(int cx, int cy, int r, int color)
{ gfx_on = 1; gfx_dirty = 1; bgfx_circle(cx, cy, r, color); }
static void bw_plot(int x, int y, int ch)
{ (void)ch; gfx_on = 1; gfx_dirty = 1; bgfx_line(x, y, x, y, 7); }   /* 1px dot */
static int  bw_gfx_active(void) { return gfx_on; }

/* Toolbar / program-button click — window-local coords (called from main.c via
 * the window_t.on_click seam on the left-button press edge). */
static void bw_on_click(window_t *self, int lx, int ly)
{
    if (self != &basic_win || !inited) return;
    basic_select(0);
    /* program BUTTONs first */
    for (int n = 0; n < PBTN_MAX; n++) {
        int bx, by, bw, bh;
        if (!pbtn_rect(self->width, n, &bx, &by, &bw, &bh)) continue;
        if (lx >= bx && lx < bx + bw && ly >= by && ly < by + bh) {
            pbtn_clicks[n]++;
            return;
        }
    }
    for (int b = 0; b < BW_NBTN; b++) {
        int bx, by, bw, bh;
        bw_btn_rect(b, self->width, &bx, &by, &bw, &bh);
        if (lx >= bx && lx < bx + bw && ly >= by && ly < by + bh) {
            bw_emit(bw_btns[b].cmd);
            bw_putc('\n');
            basic_exec_line(bw_btns[b].cmd);
            return;
        }
    }
}

void basicwin_init(void)
{
    basic_select(0);
    clear_all();
    esc_state = 0;
    gfx_on = 0;
    inited = 1;
    basic_init();
    basic_set_emit(bw_emit);
    basic_set_cls(bw_cls);
    basic_set_pause(bw_pause);
    basic_set_break_poll(bw_break_poll);
    basic_set_line(bw_line);                /* LINE   -> gfx canvas */
    basic_set_circle(bw_circle);            /* CIRCLE -> gfx canvas */
    basic_set_plot(bw_plot);                /* PLOT   -> gfx canvas */
    basic_set_gfx_active(bw_gfx_active);    /* gfx-mode flag (suppresses "Ok") */
    basic_set_button(bw_button);            /* BUTTON n,"label" -> on-screen button */
    basic_set_btn(bw_btn);                  /* BTN(n) -> clicks since last read */
    basic_set_buttons_reset(bw_buttons_reset);
    basic_win.on_click = bw_on_click;       /* wire the toolbar */
}

/* ---- drawing ---- */
static int visible_rows(window_t *self)
{
    int content_h = self->height - WM_TITLEBAR_H - BW_TOOLBAR_H - 7;
    int vr = content_h / (FONT_HEIGHT + 1);
    if (vr < 1) vr = 1;
    if (vr > BW_ROWS) vr = BW_ROWS;
    return vr;
}

static void bw_draw_toolbar(window_t *self)
{
    for (int i = 0; i < BW_NBTN; i++) {
        int bx, by, bw, bh;
        bw_btn_rect(i, self->width, &bx, &by, &bw, &bh);
        int ax = self->x + bx, ay = self->y + by;
        unsigned int bg = (i < 2) ? 0xFF2E5E8AU    /* FILES/LIST = blue  */
                                  : 0xFF1F6E2EU;   /* run "..."  = green */
        fill_rect(ax, ay, bw, bh, bg);
        int lw = bw_strlen(bw_btns[i].label) * FONT_WIDTH;
        int tx = ax + (bw - lw) / 2; if (tx < ax + 1) tx = ax + 1;
        int ty = ay + (bh - FONT_HEIGHT) / 2;
        draw_string_at(tx, ty, bw_btns[i].label, 0xFFFFFFFFU, bg);
    }
}

void basicwin_draw(window_t *self, unsigned int frame)
{
    (void)frame;
    if (self != &basic_win) return;
    basic_select(0);

    bw_draw_toolbar(self);
    bw_draw_pbtns(self);                     /* program BUTTONs (overlay strip) */
    int cx = self->x + 4;
    int cy = self->y + WM_TITLEBAR_H + BW_TOOLBAR_H + 4;
    const int line_h = FONT_HEIGHT + 1;
    int vr = visible_rows(self);

    if (cur_row < view_top) view_top = cur_row;
    if (cur_row >= view_top + vr) view_top = cur_row - vr + 1;
    if (view_top < 0) view_top = 0;

    int maxcols = (self->width - 6) / FONT_WIDTH;
    if (maxcols < 1) maxcols = 1;
    if (maxcols > BW_COLS) maxcols = BW_COLS;

    char line[BW_COLS + 1];
    for (int i = 0; i < vr; i++) {
        int r = view_top + i;
        if (r >= BW_ROWS) break;
        int n = 0;
        for (; n < maxcols && grid[r][n]; n++) line[n] = grid[r][n];
        line[n] = 0;
        draw_string_at(cx, cy + i * line_h, line, 0xFFB6FFB6U, self->content_bg);
    }

    /* block cursor at (cur_row,cur_col) if on screen */
    int crow = cur_row - view_top;
    if (crow >= 0 && crow < vr && cur_col < maxcols) {
        int px = cx + cur_col * FONT_WIDTH;
        int py = cy + crow * line_h;
        fill_rect(px, py + (FONT_HEIGHT - 2), FONT_WIDTH, 2, 0xFFFFFFFFU);
    }

    /* graphics overlay: replay the LINE/CIRCLE/PLOT display list */
    if (gfx_on) {
        int gx, gy, gw, gh;
        bw_gfx_rect(self, &gx, &gy, &gw, &gh);
        bgfx_render(gx, gy, gw, gh);
    }
}

/* ---- editing ---- */
static void insert_char(char c)
{
    for (int i = BW_COLS - 1; i > cur_col; i--) grid[cur_row][i] = grid[cur_row][i - 1];
    grid[cur_row][cur_col] = c;
    if (cur_col < BW_COLS - 1) cur_col++;
}
static void backspace(void)
{
    if (cur_col > 0) {
        for (int i = cur_col - 1; i < BW_COLS - 1; i++) grid[cur_row][i] = grid[cur_row][i + 1];
        grid[cur_row][BW_COLS - 1] = ' ';
        cur_col--;
    } else if (cur_row > view_top) {
        cur_row--; cur_col = 0;
    }
}

static void row_text(int r, char *out, int cap)
{
    int n = BW_COLS;
    while (n > 0 && grid[r][n - 1] == ' ') n--;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) out[i] = grid[r][i];
    out[n] = 0;
}

static void do_enter(void)
{
    char line[BW_COLS + 1];
    row_text(cur_row, line, sizeof line);
    cur_col = 0;
    cursor_down_one();
    if (line[0]) {
        basic_exec_line(line);
    } else if (gfx_on) {
        gfx_on = 0; bgfx_clear();        /* Enter on an empty line dismisses graphics */
    }
}

void basicwin_handle_key(char c)
{
    if (!inited) return;
    basic_select(0);

    /* --- ANSI escape parser for the arrow / nav keys --- */
    if (esc_state == 1) { esc_state = (c == '[') ? 2 : 0; return; }
    if (esc_state == 2) {
        esc_state = 0;
        switch (c) {
            case 'A': if (cur_row > 0)            cur_row--; break;   /* up    */
            case 'B': if (cur_row < BW_ROWS - 1)  cur_row++; break;   /* down  */
            case 'C': if (cur_col < BW_COLS - 1)  cur_col++; break;   /* right */
            case 'D': if (cur_col > 0)            cur_col--; break;   /* left  */
            case 'H': cur_col = 0; break;                            /* Home  */
            case 'F': { int n = BW_COLS;                             /* End   */
                        while (n > 0 && grid[cur_row][n-1] == ' ') n--;
                        cur_col = n; } break;
            case '5': if (cur_row >= 20) cur_row -= 20; else cur_row = 0; break;     /* PgUp */
            case '6': if (cur_row < BW_ROWS - 20) cur_row += 20; else cur_row = BW_ROWS-1; break; /* PgDn */
            default: break;
        }
        return;
    }
    if (c == 0x1b) { esc_state = 1; return; }

    if (c == 0x03)                      { cur_col = 0; cursor_down_one(); } /* Ctrl-C: cancel line */
    else if (c == '\r' || c == '\n')      do_enter();
    else if (c == 0x08 || c == 0x7f)      backspace();
    else if (c >= 0x20 && c < 0x7f)       insert_char(c);
}

int basicwin_is_basic(window_t *w) { return w == &basic_win; }
