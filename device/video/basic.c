// apps/basic.c — a small classic line-numbered BASIC interpreter.
//
// Modelled on the line-numbered BASIC of lecture.site44.com/basic-emu.  Runs
// as its own window in the Pi 3 gwm desktop (apps/gwm.c basic_win), but the
// interpreter core here is freestanding: ALL output goes through one callback
// and INPUT pulls a line through another, so it builds + unit-tests on a host
// with -DBASIC_HOST_TEST (gcc apps/basic.c -DBASIC_HOST_TEST -lm).
//
// Direct commands:  RUN  RUN "name"  LIST  NEW  FILES  LOAD "name"
// Statements:  PRINT (";"/","/trailing ";"/"lit"/expr)   [LET] v=expr
//              INPUT ["prompt";] v[,v...]
//              IF expr THEN (stmt | lineno)
//              GOTO n   GOSUB n   RETURN   FOR v=a TO b [STEP s]   NEXT [v]
//              END   STOP   REM ...
// Expr: + - * / ^, parens, unary -, relops = <> < > <= >=, AND OR,
//       vars A..Z and A0..Z9 (numeric), funcs ABS INT SGN SQR RND.
// Numbers are double; PRINT shows integral values without a fraction.

#ifdef BASIC_HOST_TEST
#  include <stdio.h>
#  include <string.h>
#endif
/* Newton's method sqrt — no libm dependency on the bare-metal target. */
static double b_sqrt(double x)
{
    if (x <= 0) return 0;
    double g = x > 1 ? x : 1;
    for (int i = 0; i < 50; i++) g = 0.5 * (g + x / g);
    return g;
}

/* SIN/COS via range reduction + Taylor series — no libm dependency.  Good
 * enough for graphics demos (rotate.bas line rotation). */
static double b_sin(double x)
{
    const double PI = 3.14159265358979, TWO_PI = 6.28318530717959;
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n <= 9; n++) {
        term *= -x2 / (double)((2 * n) * (2 * n + 1));
        sum  += term;
    }
    return sum;
}
static double b_cos(double x) { return b_sin(x + 1.57079632679490); }

/* ---- I/O callbacks ------------------------------------------------- */
static void (*g_emit)(const char *);
static int  (*g_input)(char *buf, int max);  /* read one line for INPUT */
static void (*g_cls)(int mode);              /* CLS[ n]: 1=text 2=gfx 3=both */
static void (*g_plot)(int x, int y, int ch); /* PLOT x,y[,c]: char at cell */
static void (*g_pause)(int ms);              /* PAUSE n: sleep n ms */
static void (*g_line)(int x1, int y1, int x2, int y2, int color); /* LINE seg */
static void (*g_circle)(int cx, int cy, int r, int color);        /* CIRCLE  */
static void (*g_wifi)(int action);           /* WIFI: 1=on 0=off 2=status   */
static int  (*g_gfx_active)(void);           /* 1 if the window is in gfx mode */
void basic_set_gfx_active(int (*fn)(void))           { g_gfx_active = fn; }
static void (*g_fullscreen)(int on);         /* maximize/restore the window */
void basic_set_fullscreen(void (*fn)(int))           { g_fullscreen = fn; }
static void (*g_button)(int n, const char *label);   /* BUTTON n,"label" */
static int  (*g_btn)(int n);                  /* BTN(n): clicks since last read */
static void (*g_buttons_reset)(void);         /* clear program buttons (on RUN) */
void basic_set_button(void (*fn)(int, const char *)) { g_button = fn; }
void basic_set_btn(int (*fn)(int))                   { g_btn = fn; }
void basic_set_buttons_reset(void (*fn)(void))       { g_buttons_reset = fn; }
/* Event-driven buttons: BUTTON n,"label",line registers a handler line that is
 * GOSUB'd automatically when button n is clicked.  g_btn_queue accumulates
 * presses so each click dispatches exactly one handler call. */
#define BTN_MAX 8
static int g_btn_handler[BTN_MAX];           /* handler line per button (0 = polled) */
static int g_btn_queue[BTN_MAX];             /* pending handler dispatches            */
static int g_has_btn_handlers = 0;
static int bas_next_btn_event(void)          /* handler line of one pending event, or 0 */
{
    if (!g_btn) return 0;
    for (int n = 0; n < BTN_MAX; n++) if (g_btn_handler[n] > 0) {
        g_btn_queue[n] += g_btn(n);          /* drain new clicks into our queue */
        if (g_btn_queue[n] > 0) { g_btn_queue[n]--; return g_btn_handler[n]; }
    }
    return 0;
}
/* True while a running program has clickable BUTTON handlers — the input layer
 * uses this so a mouse click goes to the program's buttons instead of being
 * treated as a Ctrl-C break. */
int basic_has_buttons(void) { return g_has_btn_handlers; }
void basic_set_emit(void (*fn)(const char *))        { g_emit = fn; }
void basic_set_input(int (*fn)(char *, int))         { g_input = fn; }
void basic_set_cls(void (*fn)(int))                  { g_cls = fn; }
void basic_set_plot(void (*fn)(int, int, int))       { g_plot = fn; }
void basic_set_pause(void (*fn)(int))                { g_pause = fn; }
void basic_set_line(void (*fn)(int, int, int, int, int)) { g_line = fn; }
void basic_set_circle(void (*fn)(int, int, int, int))    { g_circle = fn; }
void basic_set_wifi(void (*fn)(int))                     { g_wifi = fn; }
/* Break poll: a long RUN blocks the single-threaded wm pump, so the RUN loop
 * calls this hook periodically to notice a Ctrl-C pressed mid-run. */
static int (*g_break_poll)(void);
void basic_set_break_poll(int (*fn)(void))               { g_break_poll = fn; }
static void emit(const char *s) { if (g_emit) g_emit(s); }
static void emitc(char c) { char b[2]; b[0] = c; b[1] = 0; emit(b); }

/* ---- helpers ------------------------------------------------------- */
static int  b_isdigit(char c) { return c >= '0' && c <= '9'; }
static int  b_isalpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static char b_up(char c)      { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* double -> string: integral values print with no fraction. */
static void num_str(double v, char *out)
{
    int p = 0;
    if (v < 0) { out[p++] = '-'; v = -v; }
    long ip_ = (long)v;
    double fr = (v - (double)ip_) * 1000000.0 + 0.5;
    long frac = (long)fr;
    if (frac >= 1000000L) { frac -= 1000000L; ip_++; }
    char tmp[24]; int n = 0; long q = ip_;
    if (q == 0) tmp[n++] = '0';
    while (q > 0) { tmp[n++] = (char)('0' + q % 10); q /= 10; }
    while (n > 0) out[p++] = tmp[--n];
    if (frac != 0) {
        char fb[6]; for (int i = 5; i >= 0; i--) { fb[i] = (char)('0' + frac % 10); frac /= 10; }
        int last = 5; while (last >= 0 && fb[last] == '0') last--;
        if (last >= 0) { out[p++] = '.'; for (int i = 0; i <= last; i++) out[p++] = fb[i]; }
    }
    out[p] = 0;
}

/* ---- program + variables ------------------------------------------ */
#define MAXPROG  320
#define PLINELEN 96
#define SVAR_LEN 64
#define NVAR     224
#define ARR_POOL 4096
#define FOR_MAX 32
#define GOSUB_MAX 256          /* deep enough for recursive programs (maze) */
#define WHILE_MAX 32

/* ---- multiple independent interpreters ----------------------------------
 * Each on-screen BASIC window (apps/gwm.c) runs its own REPL thread with a
 * fully independent program + variables + runtime stacks.  All the former
 * file-scope globals now live in one struct, instanced per window in bs[].
 * The "current" instance is resolved from the running thread id (robust
 * under preemption — no shared cursor to race on); a #define redirect below
 * leaves the ~1700-line interpreter body unchanged.
 *
 * Variables are kept in a name table so multi-character names (NP, PIVOT,
 * TVX, RAD, ...) work, not just A..Z / A0..Z9.  Each slot holds a scalar
 * value, a string value (used when the name ends in '$'), and optional array
 * metadata (a name can be both a scalar and an array, classic-BASIC style).
 * DATA/READ/RESTORE cursor: data_pc = prog[] index of the DATA line being
 * read (-1 before the first READ / after RESTORE); data_ip walks its items,
 * NULL forces a re-scan from data_pc+1. */
#define NBASIC 4          /* Pi 4: up to 4 on-screen BASIC windows (1 + 3 extra) */
struct basic_state {
    struct { int no; char text[PLINELEN]; } prog[MAXPROG];
    int    nprog;
    char   vname[NVAR][12];        /* uppercased name; "" = free slot      */
    double vval[NVAR];             /* scalar numeric value                 */
    char   vstr[NVAR][SVAR_LEN];   /* string value (name ends in '$')      */
    struct { int base, d1, d2, ndim; } varr[NVAR];   /* array (ndim 0 = none) */
    int    nvar;
    double arrpool[ARR_POOL];
    int    arrtop;
    int    data_pc;
    const char *data_ip;
    /* runtime state */
    int    running;
    int    pc;                     /* current prog[] index while running   */
    const char *ip;                /* statement cursor                     */
    int    err;
    char   errmsg[64];
    int    g_goto;                 /* -1 none, -2 pc/ip already set, >=0 target line */
    volatile int brk;              /* set by Ctrl-C to interrupt a program (g_break) */
    struct { int var; double limit, step; int idx; const char *stmt; } forstk[FOR_MAX];
    int    fortop;
    struct { int rpc; const char *rip; } gosubstk[GOSUB_MAX];   /* return addr */
    int    gosubtop;
    struct { int rpc; const char *rip; } whilestk[WHILE_MAX];   /* WHILE cond  */
    int    whiletop;
    /* line debugger (`debug` command): per-instance so two BASIC windows
     * debug independently. */
    int    dbg_on;                 /* 1 while a debug session is running   */
    int    dbg_step;               /* 1 = stop at every line; 0 = run to bp */
    int    dbg_bp[16];             /* breakpoint line numbers              */
    int    dbg_nbp;
};
static struct basic_state bs[NBASIC];

/* Which instance the running thread drives.  basic_bind_thread() records the
 * tid at REPL start; the gwm.c hooks call basic_curi() to agree on it. */
static int basic_tid[NBASIC] = { -1 };
#ifdef BASIC_HOST_TEST
int basic_host_inst = 0;               /* host test: which instance is "current" */
#endif
/* Active instance for the on-screen windows.  The WM is single-threaded, so the
 * window layer (basicwin.c) calls basic_select(i) right before it dispatches a
 * line / key / draw to BASIC window i; every bs[basic_curi()] macro then resolves
 * to that window's private interpreter state. */
static int basic_cur = 0;
void basic_select(int inst) { if (inst >= 0 && inst < NBASIC) basic_cur = inst; }
int basic_curi(void)
{
#ifdef BASIC_HOST_TEST
    return basic_host_inst;
#else
    (void)basic_tid;
    return basic_cur;
#endif
}
#ifndef BASIC_HOST_TEST
void basic_bind_thread(int inst) { (void)inst; }   /* selection is explicit via basic_select() */
#endif

/* Redirect every former global to the current instance's field. */
#define prog      (bs[basic_curi()].prog)
#define nprog     (bs[basic_curi()].nprog)
#define vname     (bs[basic_curi()].vname)
#define vval      (bs[basic_curi()].vval)
#define vstr      (bs[basic_curi()].vstr)
#define varr      (bs[basic_curi()].varr)
#define nvar      (bs[basic_curi()].nvar)
#define arrpool   (bs[basic_curi()].arrpool)
#define arrtop    (bs[basic_curi()].arrtop)
#define data_pc   (bs[basic_curi()].data_pc)
#define data_ip   (bs[basic_curi()].data_ip)
#define running   (bs[basic_curi()].running)
#define pc        (bs[basic_curi()].pc)
#define ip        (bs[basic_curi()].ip)
#define err       (bs[basic_curi()].err)
#define errmsg    (bs[basic_curi()].errmsg)
#define g_goto    (bs[basic_curi()].g_goto)
#define g_break   (bs[basic_curi()].brk)
#define forstk    (bs[basic_curi()].forstk)
#define fortop    (bs[basic_curi()].fortop)
#define gosubstk  (bs[basic_curi()].gosubstk)
#define gosubtop  (bs[basic_curi()].gosubtop)
#define whilestk  (bs[basic_curi()].whilestk)
#define whiletop  (bs[basic_curi()].whiletop)
#define dbg_on    (bs[basic_curi()].dbg_on)
#define dbg_step  (bs[basic_curi()].dbg_step)
#define dbg_bp    (bs[basic_curi()].dbg_bp)
#define dbg_nbp   (bs[basic_curi()].dbg_nbp)

/* Ctrl-C: interrupt instance @inst's running program. */
void basic_break_n(int inst) { if (inst >= 0 && inst < NBASIC) bs[inst].brk = 1; }
void basic_break(void) { g_break = 1; }   /* current instance (back-compat) */
/* True while a program is executing (do_run loop).  Used to suppress input
 * dispatch during a RUN so the cursor-keeping mouse pump can't re-enter the
 * interpreter. */
int basic_is_running(void) { return running; }

/* True if a Ctrl-C break has been requested but not yet consumed by the RUN
 * loop.  Lets a long PAUSE / WAIT abort its sleep promptly. */
int basic_break_pending(void) { return g_break; }

static void berr(const char *m)
{ if (err) return; err = 1; int i = 0; for (; m[i] && i < (int)sizeof errmsg - 1; i++) errmsg[i] = m[i]; errmsg[i] = 0; }

/* ---- lexer over ip ------------------------------------------------- */
static void skipsp(void) { while (*ip == ' ' || *ip == '\t') ip++; }
static int kw(const char *k)
{
    skipsp(); const char *p = ip; int i = 0;
    while (k[i]) { if (b_up(*p) != k[i]) return 0; p++; i++; }
    if (b_isalpha(*p) || b_isdigit(*p)) return 0;
    ip = p; return 1;
}
/* Parse a variable name at ip into out[] (uppercased): a letter then
 * letters/digits, optionally ending in '$'.  Returns its length, 0 if none. */
static int var_name(char *out)
{
    int n = 0;
    skipsp();
    if (!b_isalpha(*ip)) { out[0] = 0; return 0; }
    while ((b_isalpha(*ip) || b_isdigit(*ip)) && n < 10) out[n++] = b_up(*ip++);
    if (*ip == '$' && n < 11) out[n++] = *ip++;
    out[n] = 0; return n;
}
/* Find or create the slot for @name; returns its index, or -1 if full. */
static int var_slot(const char *name)
{
    int i, k;
    for (i = 0; i < nvar; i++) {
        for (k = 0; vname[i][k] && vname[i][k] == name[k]; k++) ;
        if (vname[i][k] == 0 && name[k] == 0) return i;
    }
    if (nvar >= NVAR) { berr("too many vars"); return -1; }
    i = nvar++;
    for (k = 0; name[k] && k < 11; k++) vname[i][k] = name[k];
    vname[i][k] = 0;
    vval[i] = 0; vstr[i][0] = 0; varr[i].ndim = 0;
    return i;
}
static void var_clear_all(void) { nvar = 0; arrtop = 0; }

/* Numeric scalar variable: a name NOT ending in '$'.  Returns its slot, or
 * -1 (leaving ip unchanged) if there's no name / it's a string name. */
static int varidx(void)
{
    const char *save = ip; char nm[12]; int n = var_name(nm);
    if (n == 0 || nm[n - 1] == '$') { ip = save; return -1; }
    return var_slot(nm);
}

static double expr(void);
static void   seval(char *out, int max);   /* evaluate a string expression  */
static int    peek_is_string(void);        /* does a string factor come next? */
static double *arr_elem(int li);           /* &A(i[,j]); ip at '(' on entry  */
static void   paren_sexpr(char *out, int max);
static double paren_num(void);             /* '(' expr ')' -> number */
static double factor(void)
{
    skipsp();
    if (*ip == '(') { ip++; double v = expr(); skipsp(); if (*ip == ')') ip++; else berr("expected )"); return v; }
    if (*ip == '-') { ip++; return -factor(); }
    if (*ip == '+') { ip++; return factor(); }
    if (b_isdigit(*ip) || *ip == '.') {
        double v = 0; while (b_isdigit(*ip)) v = v * 10 + (*ip++ - '0');
        if (*ip == '.') { ip++; double f = 0.1; while (b_isdigit(*ip)) { v += (*ip++ - '0') * f; f *= 0.1; } }
        return v;
    }
    if (kw("ABS")) { double v = factor(); return v < 0 ? -v : v; }
    if (kw("INT")) { double v = factor(); long l = (long)v; if (v < 0 && (double)l != v) l--; return (double)l; }
    if (kw("SGN")) { double v = factor(); return v > 0 ? 1 : (v < 0 ? -1 : 0); }
    if (kw("SQR")) { double v = factor(); return v > 0 ? b_sqrt(v) : 0; }
    if (kw("SIN")) { return b_sin(factor()); }
    if (kw("COS")) { return b_cos(factor()); }
    if (kw("RND")) { double n = factor();          /* RND(n) -> [0,n), RND(1) -> [0,1) */
        static unsigned long s = 22695477UL;
        s = s * 1103515245UL + 12345UL;
        double r = (double)((s >> 16) & 0x7FFF) / 32768.0;
        return (n > 1) ? r * n : r; }
    /* colour-name constants (for LINE/CIRCLE ,COLOUR) -> palette index 0..15 */
    if (kw("BLACK"))   return 0;  if (kw("BLUE"))    return 1;
    if (kw("GREEN"))   return 2;  if (kw("CYAN"))    return 3;
    if (kw("RED"))     return 4;  if (kw("MAGENTA")) return 5;
    if (kw("YELLOW"))  return 6;  if (kw("WHITE"))   return 7;
    if (kw("LIME"))    return 10; if (kw("GRAY") || kw("GREY")) return 8;
    if (kw("BTN")) { double n = paren_num();      /* BTN(n): clicks on button n */
        return g_btn ? (double)g_btn((int)n) : 0; }
    if (kw("LEN")) { char s[SVAR_LEN]; paren_sexpr(s, sizeof s);
        int n = 0; while (s[n]) n++; return (double)n; }
    if (kw("ASC")) { char s[SVAR_LEN]; paren_sexpr(s, sizeof s);
        return (double)(unsigned char)s[0]; }
    if (kw("VAL")) { char s[SVAR_LEN]; paren_sexpr(s, sizeof s);
        const char *q = s; while (*q == ' ') q++;
        int neg = 0; if (*q == '-') { neg = 1; q++; } else if (*q == '+') q++;
        double v = 0, f; while (b_isdigit(*q)) v = v * 10 + (*q++ - '0');
        if (*q == '.') { q++; f = 0.1; while (b_isdigit(*q)) { v += (*q++ - '0') * f; f *= 0.1; } }
        return neg ? -v : v; }
    /* variable: NAME(...) -> array element, else scalar.  (functions + colour
     * names were matched by kw above, so a bare name here is a variable.) */
    { const char *save = ip; char nm[12]; int n = var_name(nm);
      if (n > 0 && nm[n - 1] != '$') {
          int vi = var_slot(nm); skipsp();
          if (*ip == '(') { double *e = arr_elem(vi); return e ? *e : 0; }
          return vi >= 0 ? vval[vi] : 0;
      }
      ip = save; berr("syntax"); return 0; }
}
static double power(void)
{
    double v = factor(); skipsp();
    while (*ip == '^') { ip++; int n = (int)factor(); double r = 1; for (int i = 0; i < n; i++) r *= v; v = r; skipsp(); }
    return v;
}
static double term(void)
{
    double v = power(); skipsp();
    for (;;) {
        if (*ip == '*' || *ip == '/') { char op = *ip++; double r = power();
            if (op == '*') v *= r; else { if (r == 0) berr("div0"); else v /= r; } }
        else if (kw("MOD")) { double r = power();
            long a = (long)v, b = (long)r; if (b == 0) { berr("div0"); }
            else v = (double)(a - (a / b) * b); }
        else break;
        skipsp();
    }
    return v;
}
static double addsub(void)
{
    double v = term(); skipsp();
    while (*ip == '+' || *ip == '-') { char op = *ip++; double r = term(); v = (op == '+') ? v + r : v - r; skipsp(); }
    return v;
}
static double relexpr(void)
{
    if (peek_is_string()) {                      /* string relational compare */
        char l[SVAR_LEN]; seval(l, sizeof l); skipsp();
        char a = *ip, b = ip[1]; int op = 0;
        if (a == '<' && b == '=') { op = 4; ip += 2; }
        else if (a == '>' && b == '=') { op = 5; ip += 2; }
        else if (a == '<' && b == '>') { op = 6; ip += 2; }
        else if (a == '<') { op = 1; ip++; }
        else if (a == '>') { op = 2; ip++; }
        else if (a == '=') { op = 3; ip++; }
        if (!op) { berr("string relop"); return 0; }
        char r[SVAR_LEN]; seval(r, sizeof r);
        const char *x = l, *y = r; while (*x && *x == *y) { x++; y++; }
        int c = (int)(unsigned char)*x - (int)(unsigned char)*y;
        switch (op) { case 1: return c < 0; case 2: return c > 0; case 3: return c == 0;
                      case 4: return c <= 0; case 5: return c >= 0; default: return c != 0; }
    }
    double v = addsub(); skipsp();
    char a = *ip, b = ip[1]; int op = 0;
    if (a == '<' && b == '=') { op = 4; ip += 2; }
    else if (a == '>' && b == '=') { op = 5; ip += 2; }
    else if (a == '<' && b == '>') { op = 6; ip += 2; }
    else if (a == '<') { op = 1; ip++; }
    else if (a == '>') { op = 2; ip++; }
    else if (a == '=') { op = 3; ip++; }
    if (op) { double r = addsub();
        switch (op) { case 1: return v < r; case 2: return v > r; case 3: return v == r;
                      case 4: return v <= r; case 5: return v >= r; default: return v != r; } }
    return v;
}
static double expr(void)
{
    double v = relexpr(); skipsp();
    for (;;) { if (kw("AND")) { double r = relexpr(); v = (v != 0 && r != 0); }
               else if (kw("OR")) { double r = relexpr(); v = (v != 0 || r != 0); }
               else break; skipsp(); }
    return v;
}

/* ---- string variables, string expressions ------------------------- */

/* Parse a string-variable name (letter, optional digit, '$') at ip.
 * Returns the 0..285 slot index and consumes it, or -1 (ip unchanged). */
static int svaridx(void)
{
    const char *save = ip; char nm[12]; int n = var_name(nm);
    if (n == 0 || nm[n - 1] != '$') { ip = save; return -1; }
    return var_slot(nm);
}

/* True if the next factor is a string (literal, A$/A1$ var, or a $-suffixed
 * function like MID$): a run of letters [+ one digit] ending in '$'. */
static int peek_is_string(void)
{
    skipsp();
    if (*ip == '"') return 1;
    const char *p = ip;
    if (!b_isalpha(*p)) return 0;
    while (b_isalpha(*p)) p++;
    if (b_isdigit(*p)) p++;
    return *p == '$';
}

static void scopy(char *out, int max, const char *src)
{
    int n = 0; while (src[n] && n < max - 1) { out[n] = src[n]; n++; } out[n] = 0;
}

/* string '(' numeric-or-string ')' helper used by LEN/ASC/VAL/CHR$/STR$ */
static void paren_sexpr(char *out, int max)
{
    skipsp(); int par = 0; if (*ip == '(') { ip++; par = 1; }
    seval(out, max);
    skipsp(); if (par) { if (*ip == ')') ip++; else berr("expected )"); }
}
static double paren_num(void)
{
    skipsp(); int par = 0; if (*ip == '(') { ip++; par = 1; }
    double v = expr();
    skipsp(); if (par) { if (*ip == ')') ip++; else berr("expected )"); }
    return v;
}

static void sfactor(char *out, int max)
{
    skipsp(); out[0] = 0;
    if (*ip == '"') { int n = 0; ip++; while (*ip && *ip != '"' && n < max - 1) out[n++] = *ip++;
                      out[n] = 0; if (*ip == '"') ip++; return; }
    if (*ip == '(') { ip++; seval(out, max); skipsp(); if (*ip == ')') ip++; else berr("expected )"); return; }
    if (kw("MID$")) {                                   /* MID$(s, start [, len]) */
        char s[SVAR_LEN]; skipsp(); if (*ip == '(') ip++; else berr("MID$ (");
        seval(s, sizeof s); skipsp(); if (*ip == ',') ip++; else berr("MID$ ,");
        int start = (int)expr(); int len = max; skipsp();
        if (*ip == ',') { ip++; len = (int)expr(); skipsp(); }
        if (*ip == ')') ip++; else berr("MID$ )");
        int slen = 0; while (s[slen]) slen++;
        int i = start - 1; if (i < 0) i = 0; int n = 0;
        while (i < slen && n < len && n < max - 1) out[n++] = s[i++]; out[n] = 0; return;
    }
    if (kw("LEFT$")) {                                  /* LEFT$(s, n) */
        char s[SVAR_LEN]; skipsp(); if (*ip == '(') ip++; else berr("LEFT$ (");
        seval(s, sizeof s); skipsp(); if (*ip == ',') ip++; else berr("LEFT$ ,");
        int len = (int)expr(); skipsp(); if (*ip == ')') ip++; else berr("LEFT$ )");
        int n = 0; while (s[n] && n < len && n < max - 1) { out[n] = s[n]; n++; } out[n] = 0; return;
    }
    if (kw("RIGHT$")) {                                 /* RIGHT$(s, n) */
        char s[SVAR_LEN]; skipsp(); if (*ip == '(') ip++; else berr("RIGHT$ (");
        seval(s, sizeof s); skipsp(); if (*ip == ',') ip++; else berr("RIGHT$ ,");
        int len = (int)expr(); skipsp(); if (*ip == ')') ip++; else berr("RIGHT$ )");
        int slen = 0; while (s[slen]) slen++;
        int i = slen - len; if (i < 0) i = 0; int n = 0;
        while (s[i] && n < max - 1) out[n++] = s[i++]; out[n] = 0; return;
    }
    if (kw("CHR$")) { int c = (int)paren_num(); out[0] = (char)c; out[1] = 0; return; }
    if (kw("STR$")) { double v = paren_num(); num_str(v, out); return; }
    { const char *save = ip; int si = svaridx(); if (si >= 0) { scopy(out, max, vstr[si]); return; } ip = save; }
    berr("string expr"); out[0] = 0;
}

static void seval(char *out, int max)                   /* '+' concatenation */
{
    sfactor(out, max); skipsp();
    while (*ip == '+') { ip++; char rhs[SVAR_LEN]; sfactor(rhs, sizeof rhs);
        int n = 0; while (out[n]) n++;
        for (int i = 0; rhs[i] && n < max - 1; i++) out[n++] = rhs[i]; out[n] = 0; skipsp(); }
}

/* ---- arrays -------------------------------------------------------- */

/* Return &A(i[,j]).  ip is positioned at '(' on entry.  Auto-dimensions an
 * undeclared letter to (10).  Sets err + returns NULL on overflow / bad
 * subscript. */
static double *arr_elem(int li)   /* li = variable slot; ip at '(' on entry */
{
    if (li < 0) return 0;
    if (varr[li].ndim == 0) {                           /* auto-DIM to (10) */
        if (arrtop + 11 > ARR_POOL) { berr("array space"); return 0; }
        varr[li].base = arrtop; varr[li].d1 = 11; varr[li].d2 = 1; varr[li].ndim = 1;
        for (int k = 0; k < 11; k++) arrpool[arrtop + k] = 0; arrtop += 11;
    }
    if (*ip == '(') ip++; else { berr("array ("); return 0; }
    int i1 = (int)expr(), i2 = 0; skipsp();
    if (*ip == ',') { ip++; i2 = (int)expr(); skipsp(); }
    if (*ip == ')') ip++; else { berr("array )"); return 0; }
    if (i1 < 0 || i1 >= varr[li].d1 || i2 < 0 || i2 >= varr[li].d2) { berr("subscript"); return 0; }
    return &arrpool[varr[li].base + i1 * varr[li].d2 + i2];
}

/* ---- DATA / READ --------------------------------------------------- */

/* Does text t begin with keyword k (case-insensitive), not glued to more
 * letters/digits?  Used to scan ahead for WHILE/WEND nesting. */
static int b_streqi_kw(const char *t, const char *k)
{
    int i = 0; while (k[i]) { if (b_up(t[i]) != k[i]) return 0; i++; }
    return !(b_isalpha(t[i]) || b_isdigit(t[i]));
}

/* Is prog[i].text a DATA statement?  (leading spaces then DATA keyword) */
static int line_is_data(const char *t)
{
    while (*t == ' ' || *t == '\t') t++;
    const char *k = "DATA"; int i = 0;
    while (k[i]) { if (b_up(*t) != k[i]) return 0; t++; i++; }
    return !(b_isalpha(*t) || b_isdigit(*t));
}

/* Fetch the next DATA item into out[].  Returns 0 when DATA is exhausted. */
static int data_next(char *out, int max)
{
    for (;;) {
        if (data_ip == 0 || *data_ip == 0) {
            int start = (data_pc < 0) ? 0 : data_pc + 1, found = -1;
            for (int i = start; i < nprog; i++) if (line_is_data(prog[i].text)) { found = i; break; }
            if (found < 0) return 0;
            data_pc = found; const char *t = prog[found].text;
            while (*t == ' ' || *t == '\t') t++; t += 4;     /* skip "DATA" */
            data_ip = t;
        }
        while (*data_ip == ' ' || *data_ip == '\t' || *data_ip == ',') data_ip++;
        if (*data_ip == 0) continue;
        int n = 0;
        if (*data_ip == '"') { data_ip++; while (*data_ip && *data_ip != '"' && n < max - 1) out[n++] = *data_ip++;
                               if (*data_ip == '"') data_ip++; }
        else { while (*data_ip && *data_ip != ',' && n < max - 1) out[n++] = *data_ip++;
               while (n > 0 && (out[n-1] == ' ' || out[n-1] == '\t')) n--; }
        out[n] = 0; return 1;
    }
}
static double str_to_num(const char *q)
{
    while (*q == ' ') q++; int neg = 0;
    if (*q == '-') { neg = 1; q++; } else if (*q == '+') q++;
    double v = 0, f; while (b_isdigit(*q)) v = v * 10 + (*q++ - '0');
    if (*q == '.') { q++; f = 0.1; while (b_isdigit(*q)) { v += (*q++ - '0') * f; f *= 0.1; } }
    return neg ? -v : v;
}

/* ---- program edit -------------------------------------------------- */
static int find_line(int no) { for (int i = 0; i < nprog; i++) if (prog[i].no == no) return i; return -1; }

/* Line number of the line whose text is `*name` (case-insensitive), or -1. */
static int find_label(const char *name)
{
    for (int i = 0; i < nprog; i++) {
        const char *t = prog[i].text; int k;
        if (*t != '*') continue; t++;
        for (k = 0; name[k] && b_up(t[k]) == name[k]; k++) ;
        if (name[k] == 0 && !(b_isalpha(t[k]) || b_isdigit(t[k]))) return prog[i].no;
    }
    return -1;
}
/* Parse a GOTO/GOSUB target: a line number, or *LABEL.  Returns the line
 * number (do_run maps it to a prog[] index), or -1 if a label isn't found. */
static int goto_target(void)
{
    skipsp();
    if (*ip == '*') {
        char nm[12]; int n = 0; ip++;
        while ((b_isalpha(*ip) || b_isdigit(*ip)) && n < 11) nm[n++] = b_up(*ip++);
        nm[n] = 0;
        return find_label(nm);
    }
    return (int)expr();
}
static void prog_set(int no, const char *text)
{
    int empty = 1; for (const char *p = text; *p; p++) if (*p != ' ' && *p != '\t') { empty = 0; break; }
    int i = find_line(no);
    if (i >= 0) {
        if (empty) { for (int j = i; j < nprog - 1; j++) prog[j] = prog[j + 1]; nprog--; return; }
        int k = 0; for (; text[k] && k < PLINELEN - 1; k++) prog[i].text[k] = text[k]; prog[i].text[k] = 0; return;
    }
    if (empty) return;
    if (nprog >= MAXPROG) { emit("?too many lines\n"); return; }
    int pos = nprog; for (int j = 0; j < nprog; j++) if (prog[j].no > no) { pos = j; break; }
    for (int j = nprog; j > pos; j--) prog[j] = prog[j - 1];
    prog[pos].no = no;
    int k = 0; for (; text[k] && k < PLINELEN - 1; k++) prog[pos].text[k] = text[k]; prog[pos].text[k] = 0;
    nprog++;
}
/* Uppercase the leading alphabetic token of @t (after any spaces) into @out. */
static void bas_first_token(const char *t, char *out, int cap)
{
    while (*t == ' ' || *t == '\t') t++;
    int n = 0;
    while (n < cap - 1 && ((*t >= 'A' && *t <= 'Z') || (*t >= 'a' && *t <= 'z'))) {
        char c = *t++; if (c >= 'a') c -= 32; out[n++] = c;
    }
    out[n] = 0;
}
static int bas_streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; } return *a == *b;
}

/* Pretty-printing LIST: right-align line numbers and indent FOR/WHILE blocks
 * (NEXT/WEND de-indent), so nested loops read at a glance. */
static void do_list(void)
{
    char nb[16], tok[8];
    int depth = 0;
    for (int i = 0; i < nprog; i++) {
        const char *t = prog[i].text;
        const char *p = t; while (*p == ' ' || *p == '\t') p++;  /* trim leading */
        bas_first_token(t, tok, sizeof tok);
        int is_close = bas_streq(tok, "NEXT") || bas_streq(tok, "WEND");
        int is_open  = bas_streq(tok, "FOR")  || bas_streq(tok, "WHILE");
        if (is_close && depth > 0) depth--;

        num_str((double)prog[i].no, nb);
        int ln = 0; while (nb[ln]) ln++;
        for (int s = ln; s < 4; s++) emit(" ");        /* right-align to width 4 */
        emit(nb); emit("  ");
        for (int d = 0; d < depth && d < 12; d++) emit("  ");
        emit(p); emit("\n");

        if (is_open) depth++;
    }
}

/* ---- pre-loaded sample programs (read-only) ----------------------- *
 * Text-only samples from lecture.site44.com/basic-emu.  The graphics
 * samples there (line/circle/star/...) are omitted because this BASIC
 * has no graphics output — only the computational/PRINT ones are kept. */
static const char *S_hello[]   = { "10 PRINT \"HELLO, WORLD!\"", "20 PRINT \"WELCOME TO BASIC.\"" };
static const char *S_forloop[] = { "10 FOR I=1 TO 10", "20 PRINT I", "30 NEXT" };
static const char *S_mtable[]  = { "10 FOR I=1 TO 9", "20 K=7*I", "30 PRINT K", "40 NEXT" };
static const char *S_add[]     = { "10 A=10", "20 B=20", "30 C=A+B", "40 PRINT C" };
static const char *S_sum100[]  = { "10 S=0", "20 FOR I=1 TO 100", "30 S=S+I", "40 NEXT", "50 PRINT S" };
static const char *S_fibon[]   = { "10 A=0", "20 B=1", "30 FOR I=1 TO 12", "40 PRINT A", "50 C=A+B", "60 A=B", "70 B=C", "80 NEXT" };
static const char *S_squares[] = { "10 FOR I=1 TO 10", "20 K=I*I", "30 PRINT K", "40 NEXT" };
/* rotate.bas — spin a line segment about the centre of the graphics screen,
 * ten times, drawing it with the LINE statement (CLS + LINE + SIN/COS). */
static const char *S_rotate[] = {
    "10 REM ROTATING LINE SEGMENT",
    "20 FOR N=1 TO 10",
    "30 FOR A=0 TO 170 STEP 10",
    "40 CLS 2",
    "50 R=A*3.14159/180",
    "60 X1=200-130*COS(R) : Y1=150-130*SIN(R)",
    "70 X2=200+130*COS(R) : Y2=150+130*SIN(R)",
    "80 LINE(X1,Y1)-(X2,Y2),5",
    "90 PAUSE 80",
    "100 NEXT",
    "110 NEXT",
    "120 END"
};

/* New-feature demos: strings, arrays (DIM), WHILE/WEND + MOD, DATA/READ. */
static const char *S_strings[] = {
    "10 A$=\"HELLO\"", "20 B$=\"WORLD\"", "30 C$=A$+\", \"+B$+\"!\"",
    "40 PRINT C$", "50 PRINT \"LENGTH=\";LEN(C$)",
    "60 PRINT \"UPPER 5: \";LEFT$(C$,5)", "70 PRINT \"MID: \";MID$(C$,8,5)" };
static const char *S_bsort[] = {       /* bubble sort, line-bar visualisation */
    "10 CLS 3",
    "20 N = 100",
    "30 DIM A(100)",
    "40 FOR I = 0 TO N - 1",
    "50 A(I) = INT(RND(100)) + 1",
    "60 NEXT",
    "70 PRINT \"BUBBLE SORT - 100 ELEMENTS\"",
    "80 GOSUB *DRAWALL",
    "90 WAIT",
    "100 FOR I = 0 TO N - 2",
    "110 FOR J = 0 TO N - 2 - I",
    "120 IF A(J) <= A(J+1) THEN GOTO 170",
    "130 T = A(J)",
    "140 A(J) = A(J+1)",
    "150 A(J+1) = T",
    "170 NEXT",
    "180 GOSUB *DRAWALL",
    "190 WAIT 0.03",
    "200 NEXT",
    "210 PRINT \"DONE!\"",
    "220 GOSUB *DRAWALL",
    "230 END",
    "800 *DRAWALL",
    "810 CLS 2",
    "820 FOR K = 0 TO N - 1",
    "830 V = A(K)",
    "840 BX = 20 + K * 5",
    "850 BY = 270 - V * 2",
    "860 LINE (BX,BY)-(BX,270),CYAN",
    "870 NEXT",
    "880 RETURN"
};
static const char *S_fizz[] = {                  /* FizzBuzz: MOD + multi-stmt */
    "10 FOR N=1 TO 20",
    "20 IF N MOD 15=0 THEN PRINT \"FIZZBUZZ\" : GOTO 60",
    "30 IF N MOD 3=0 THEN PRINT \"FIZZ\" : GOTO 60",
    "40 IF N MOD 5=0 THEN PRINT \"BUZZ\" : GOTO 60",
    "50 PRINT N", "60 NEXT" };
static const char *S_table[] = {                 /* DATA/READ name=value table */
    "10 FOR I=1 TO 3", "20 READ N$,V", "30 PRINT N$;\" = \";V", "40 NEXT",
    "50 DATA \"APPLE\",10,\"BANANA\",20,\"CHERRY\",30" };
static const char *S_count[] = {                 /* WHILE/WEND countdown */
    "10 N=10", "20 WHILE N>0", "30 PRINT N;\" \";", "40 N=N-1", "50 WEND",
    "60 PRINT \"LIFTOFF!\"" };

/* ---- ported from lecture.site44.com/basic-emu (yaskodama/basic-emulator) ---- */
static const char *S_maze[] = {
    "10 CLS 3",
    "20 W = 12",
    "30 H = 10",
    "40 CS = 30",
    "50 OX = 140",
    "60 OY = 70",
    "70 NC = W * H",
    "80 PRINT \"GENERATING MAZE...\"",
    "90 DIM WN(120)",
    "95 DIM WL(120)",
    "100 DIM VS(120)",
    "105 DIM SX(150)",
    "110 DIM SY(150)",
    "115 DIM NDR(4)",
    "120 FOR I = 0 TO NC - 1",
    "130 WN(I) = 1",
    "140 WL(I) = 1",
    "150 VS(I) = 0",
    "160 NEXT",
    "170 X = 0",
    "180 Y = 0",
    "190 SP = 0",
    "200 GOSUB *GENERATE",
    "210 CLS 3",
    "220 GOSUB *DRAWMAZE",
    "230 PRINT \"MAZE GENERATED. SOLVING...\"",
    "240 FOR I = 0 TO NC - 1",
    "250 VS(I) = 0",
    "260 NEXT",
    "270 SX(0) = 0",
    "280 SY(0) = 0",
    "290 SP = 1",
    "300 X = 0",
    "310 Y = 0",
    "320 TRIAL = 0",
    "330 FOUND = 0",
    "340 GIDX = (H - 1) * W + (W - 1)",
    "350 GOSUB *SOLVE",
    "360 IF FOUND = 0 THEN GOTO 1700",
    "370 PRINT \"SOLVED!\"",
    "380 PRINT \"TRIALS: TRIAL\"",
    "390 FOR PI = 0 TO SP - 2",
    "400 PX = OX + SX(PI) * CS + CS / 2",
    "410 PY = OY + SY(PI) * CS + CS / 2",
    "420 PX2 = OX + SX(PI + 1) * CS + CS / 2",
    "430 PY2 = OY + SY(PI + 1) * CS + CS / 2",
    "440 LINE (PX,PY)-(PX2,PY2),YELLOW",
    "450 NEXT",
    "460 LINE (OX-12,OY+CS/2)-(OX+CS/2,OY+CS/2),YELLOW",
    "470 PX = OX + (W-1) * CS + CS / 2",
    "480 PY = OY + (H-1) * CS + CS / 2",
    "490 LINE (PX,PY)-(OX+W*CS+12,PY),YELLOW",
    "500 END",
    "1700 PRINT \"NO SOLUTION\"",
    "1710 END",
    "2000 *GENERATE",
    "2010 VS(Y * W + X) = 1",
    "2020 NB = 0",
    "2030 IF Y = 0 THEN GOTO 2070",
    "2040 IDX = (Y - 1) * W + X",
    "2050 IF VS(IDX) = 1 THEN GOTO 2070",
    "2060 NDR(NB) = 0",
    "2065 NB = NB + 1",
    "2070 IF X = W - 1 THEN GOTO 2110",
    "2080 IDX = Y * W + X + 1",
    "2090 IF VS(IDX) = 1 THEN GOTO 2110",
    "2100 NDR(NB) = 1",
    "2105 NB = NB + 1",
    "2110 IF Y = H - 1 THEN GOTO 2150",
    "2120 IDX = (Y + 1) * W + X",
    "2130 IF VS(IDX) = 1 THEN GOTO 2150",
    "2140 NDR(NB) = 2",
    "2145 NB = NB + 1",
    "2150 IF X = 0 THEN GOTO 2190",
    "2160 IDX = Y * W + X - 1",
    "2170 IF VS(IDX) = 1 THEN GOTO 2190",
    "2180 NDR(NB) = 3",
    "2185 NB = NB + 1",
    "2190 IF NB = 0 THEN RETURN",
    "2200 R = INT(RND(NB))",
    "2210 D = NDR(R)",
    "2220 NX = X",
    "2230 NY = Y",
    "2240 IF D = 0 THEN NY = Y - 1",
    "2245 IF D = 0 THEN WN(Y * W + X) = 0",
    "2250 IF D = 1 THEN NX = X + 1",
    "2255 IF D = 1 THEN WL(Y * W + X + 1) = 0",
    "2260 IF D = 2 THEN NY = Y + 1",
    "2265 IF D = 2 THEN WN((Y + 1) * W + X) = 0",
    "2270 IF D = 3 THEN NX = X - 1",
    "2275 IF D = 3 THEN WL(Y * W + X) = 0",
    "2280 SX(SP) = X",
    "2290 SY(SP) = Y",
    "2300 SP = SP + 1",
    "2310 X = NX",
    "2320 Y = NY",
    "2330 GOSUB *GENERATE",
    "2340 SP = SP - 1",
    "2350 X = SX(SP)",
    "2360 Y = SY(SP)",
    "2370 GOTO 2020",
    "3000 *SOLVE",
    "3010 TRIAL = TRIAL + 1",
    "3020 VS(Y * W + X) = 1",
    "3030 GOSUB *DRAWMARK",
    "3040 IF Y * W + X = GIDX THEN GOTO 3300",
    "3050 IF Y = 0 THEN GOTO 3100",
    "3055 IF WN(Y * W + X) = 1 THEN GOTO 3100",
    "3060 IDX = (Y - 1) * W + X",
    "3070 IF VS(IDX) = 1 THEN GOTO 3100",
    "3080 NX = X",
    "3085 NY = Y - 1",
    "3090 GOSUB *DESCEND",
    "3095 IF FOUND = 1 THEN RETURN",
    "3100 IF X = W - 1 THEN GOTO 3150",
    "3105 IF WL(Y * W + X + 1) = 1 THEN GOTO 3150",
    "3110 IDX = Y * W + X + 1",
    "3120 IF VS(IDX) = 1 THEN GOTO 3150",
    "3130 NX = X + 1",
    "3135 NY = Y",
    "3140 GOSUB *DESCEND",
    "3145 IF FOUND = 1 THEN RETURN",
    "3150 IF Y = H - 1 THEN GOTO 3200",
    "3155 IF WN((Y + 1) * W + X) = 1 THEN GOTO 3200",
    "3160 IDX = (Y + 1) * W + X",
    "3170 IF VS(IDX) = 1 THEN GOTO 3200",
    "3180 NX = X",
    "3185 NY = Y + 1",
    "3190 GOSUB *DESCEND",
    "3195 IF FOUND = 1 THEN RETURN",
    "3200 IF X = 0 THEN GOTO 3250",
    "3205 IF WL(Y * W + X) = 1 THEN GOTO 3250",
    "3210 IDX = Y * W + X - 1",
    "3220 IF VS(IDX) = 1 THEN GOTO 3250",
    "3230 NX = X - 1",
    "3235 NY = Y",
    "3240 GOSUB *DESCEND",
    "3245 IF FOUND = 1 THEN RETURN",
    "3250 RETURN",
    "3300 FOUND = 1",
    "3310 RETURN",
    "4000 *DESCEND",
    "4010 SX(SP) = NX",
    "4020 SY(SP) = NY",
    "4030 SP = SP + 1",
    "4040 X = NX",
    "4050 Y = NY",
    "4060 WAIT 0.02",
    "4070 GOSUB *SOLVE",
    "4080 IF FOUND = 1 THEN RETURN",
    "4090 SP = SP - 1",
    "4100 X = SX(SP - 1)",
    "4110 Y = SY(SP - 1)",
    "4120 RETURN",
    "5000 *DRAWMAZE",
    "5010 LINE (OX,OY)-(OX+W*CS,OY),WHITE",
    "5020 LINE (OX,OY+CS)-(OX,OY+H*CS),WHITE",
    "5030 LINE (OX+W*CS,OY)-(OX+W*CS,OY+(H-1)*CS),WHITE",
    "5040 LINE (OX,OY+H*CS)-(OX+W*CS,OY+H*CS),WHITE",
    "5050 FOR DI = 0 TO H - 1",
    "5060 FOR DJ = 0 TO W - 1",
    "5070 IDX = DI * W + DJ",
    "5080 IF DI = 0 THEN GOTO 5110",
    "5090 IF WN(IDX) = 0 THEN GOTO 5110",
    "5100 LINE (OX+DJ*CS,OY+DI*CS)-(OX+(DJ+1)*CS,OY+DI*CS),WHITE",
    "5110 IF DJ = 0 THEN GOTO 5140",
    "5120 IF WL(IDX) = 0 THEN GOTO 5140",
    "5130 LINE (OX+DJ*CS,OY+DI*CS)-(OX+DJ*CS,OY+(DI+1)*CS),WHITE",
    "5140 NEXT",
    "5150 NEXT",
    "5160 CIRCLE (OX-12,OY+CS/2),5,GREEN",
    "5170 LINE (OX-7,OY+CS/2)-(OX,OY+CS/2),GREEN",
    "5180 CIRCLE (OX+W*CS+12,OY+(H-1)*CS+CS/2),5,RED",
    "5190 LINE (OX+W*CS,OY+(H-1)*CS+CS/2)-(OX+W*CS+7,OY+(H-1)*CS+CS/2),RED",
    "5200 RETURN",
    "6000 *DRAWMARK",
    "6010 PX = OX + X * CS + CS / 2",
    "6020 PY = OY + Y * CS + CS / 2",
    "6030 CIRCLE (PX,PY),3,MAGENTA",
    "6040 RETURN"
};
static const char *S_glass[] = {
    "10 CLS 3",
    "20 NP = 8",
    "30 NA = 12",
    "40 DIM YP(7)",
    "50 DIM RP(7)",
    "60 DIM VX(95)",
    "70 DIM VY(95)",
    "80 DIM VK(95)",
    "90 DIM CT(11)",
    "100 DIM ST(11)",
    "110 YP(0) = 0",
    "120 RP(0) = 0.6",
    "130 YP(1) = 0.1",
    "140 RP(1) = 0.6",
    "150 YP(2) = 0.15",
    "160 RP(2) = 0.08",
    "170 YP(3) = 1.1",
    "180 RP(3) = 0.08",
    "190 YP(4) = 1.2",
    "200 RP(4) = 0.3",
    "210 YP(5) = 1.5",
    "220 RP(5) = 0.5",
    "230 YP(6) = 1.9",
    "240 RP(6) = 0.55",
    "250 YP(7) = 2.2",
    "260 RP(7) = 0.45",
    "270 PI = 3.14159265",
    "275 FOR J = 0 TO NA - 1",
    "280 CT(J) = COS(J * 2 * PI / NA)",
    "285 ST(J) = SIN(J * 2 * PI / NA)",
    "290 NEXT",
    "310 T = 5",
    "320 *FRAME",
    "330 CLS 2",
    "340 RX = T * 0.06",
    "342 RY = T * 0.09",
    "344 RZ = T * 0.04",
    "346 CXA = COS(RX) : SXA = SIN(RX)",
    "348 CYA = COS(RY) : SYA = SIN(RY)",
    "350 CZA = COS(RZ) : SZA = SIN(RZ)",
    "400 FOR I = 0 TO NP - 1",
    "410 RI = RP(I)",
    "420 BY = YP(I) - 1.1",
    "430 FOR J = 0 TO NA - 1",
    "440 BX = RI * CT(J)",
    "445 BZ = RI * ST(J)",
    "450 Y1 = BY * CXA - BZ * SXA",
    "455 Z1 = BY * SXA + BZ * CXA",
    "460 X2 = BX * CYA + Z1 * SYA",
    "465 Z2 = -BX * SYA + Z1 * CYA",
    "470 WX = X2 * CZA - Y1 * SZA",
    "475 WY = X2 * SZA + Y1 * CZA",
    "480 WZ = Z2 + 3",
    "490 GOSUB *PROJ",
    "500 IDX = I * NA + J",
    "510 VX(IDX) = SX",
    "520 VY(IDX) = SY",
    "530 VK(IDX) = OK",
    "540 NEXT",
    "550 NEXT",
    "560 FOR I = 0 TO NP - 1",
    "570 FOR J = 0 TO NA - 1",
    "580 J2 = (J + 1) MOD NA",
    "590 K1 = I * NA + J",
    "600 K2 = I * NA + J2",
    "610 IF VK(K1) = 1 THEN IF VK(K2) = 1 THEN LINE (VX(K1),VY(K1))-(VX(K2),VY(K2)),MAGENTA",
    "620 NEXT",
    "630 NEXT",
    "640 FOR I = 0 TO NP - 2",
    "650 FOR J = 0 TO NA - 1",
    "660 K1 = I * NA + J",
    "670 K2 = (I + 1) * NA + J",
    "680 IF VK(K1) = 1 THEN IF VK(K2) = 1 THEN LINE (VX(K1),VY(K1))-(VX(K2),VY(K2)),CYAN",
    "690 NEXT",
    "700 NEXT",
    "710 T = T + 1",
    "720 WAIT 0.03",
    "730 GOTO *FRAME",
    "740 *PROJ",
    "750 OK = 1",
    "760 IF WZ < 0.5 THEN OK = 0",
    "770 IF OK = 0 THEN RETURN",
    "780 SX = 320 + 200 * WX / WZ",
    "790 SY = 200 - 200 * WY / WZ",
    "800 RETURN",
};
static const char *S_qsort[] = {
    "10 CLS 3",
    "20 N = 100",
    "30 DIM A(100)",
    "40 DIM SL(200)",
    "50 DIM SH(200)",
    "60 FOR I = 0 TO N - 1",
    "70 A(I) = INT(RND(100)) + 1",
    "80 NEXT",
    "90 PRINT \"QUICKSORT - 100 ELEMENTS\"",
    "100 GOSUB *DRAWALL",
    "110 WAIT",
    "120 SP = 0",
    "130 SL(SP) = 0",
    "140 SH(SP) = N - 1",
    "150 SP = SP + 1",
    "160 IF SP = 0 THEN GOTO 350",
    "170 SP = SP - 1",
    "180 LOW = SL(SP)",
    "190 HIGH = SH(SP)",
    "200 IF LOW >= HIGH THEN GOTO 160",
    "210 GOSUB *PART",
    "220 SL(SP) = LOW",
    "230 SH(SP) = P - 1",
    "240 SP = SP + 1",
    "250 SL(SP) = P + 1",
    "260 SH(SP) = HIGH",
    "270 SP = SP + 1",
    "280 GOSUB *DRAWALL",
    "290 WAIT 0.05",
    "300 GOTO 160",
    "350 PRINT \"DONE!\"",
    "360 GOSUB *DRAWALL",
    "370 END",
    "500 *PART",
    "510 PIVOT = A(HIGH)",
    "520 I = LOW - 1",
    "530 FOR J = LOW TO HIGH - 1",
    "540 IF A(J) <= PIVOT THEN GOSUB *INCSWAP",
    "550 NEXT",
    "560 I = I + 1",
    "570 T = A(I)",
    "580 A(I) = A(HIGH)",
    "590 A(HIGH) = T",
    "600 P = I",
    "610 RETURN",
    "700 *INCSWAP",
    "710 I = I + 1",
    "720 T = A(I)",
    "730 A(I) = A(J)",
    "740 A(J) = T",
    "750 RETURN",
    "800 *DRAWALL",
    "810 CLS 2",
    "820 FOR K = 0 TO N - 1",
    "830 V = A(K)",
    "840 BX = 20 + K * 5",
    "850 BY = 270 - V * 2",
    "860 LINE (BX,BY)-(BX,270),CYAN",
    "870 NEXT",
    "880 RETURN"
};
static const char *S_bubble[] = {
    "10 CLS 3",
    "20 N = 100",
    "30 DIM A(100)",
    "40 FOR I = 0 TO N - 1",
    "50 A(I) = INT(RND(100)) + 1",
    "60 NEXT",
    "70 PRINT \"BUBBLE SORT - 100 ELEMENTS\"",
    "80 GOSUB *DRAWALL",
    "90 WAIT",
    "100 FOR I = 0 TO N - 2",
    "110 FOR J = 0 TO N - 2 - I",
    "120 IF A(J) > A(J+1) THEN GOSUB *SWAP",
    "130 NEXT",
    "140 GOSUB *DRAWALL",
    "150 WAIT 0.05",
    "160 NEXT",
    "170 REM keep the bars on screen",
    "180 END",
    "500 *SWAP",
    "510 T = A(J)",
    "520 A(J) = A(J+1)",
    "530 A(J+1) = T",
    "540 RETURN",
    "700 *DRAWALL",
    "710 CLS 2",
    "720 FOR K = 0 TO N - 1",
    "730 V = A(K)",
    "740 BX = 30 + K * 6",
    "750 BY = 290 - V * 2",
    "760 LINE (BX,BY)-(BX,290),CYAN",
    "770 NEXT",
    "780 RETURN"
};
static const char *S_koch[] = {
    "10 CLS 3",
    "20 D = 4",
    "30 BUTTON 0, \"Level -\", 300",
    "40 BUTTON 1, \"Level +\", 350",
    "50 GOSUB *SHOW",
    "60 GOSUB *REDRAW",
    "70 *LOOP",
    "80 WAIT 0.02",
    "90 GOTO *LOOP",
    "300 D = D - 1",
    "310 IF D < 0 THEN D = 0",
    "320 GOSUB *SHOW",
    "330 GOSUB *REDRAW",
    "340 RETURN",
    "350 D = D + 1",
    "360 IF D > 7 THEN D = 7",
    "370 GOSUB *SHOW",
    "380 GOSUB *REDRAW",
    "390 RETURN",
    "400 *SHOW",
    "410 BUTTON 2, \"LEVEL \" + STR$(D)",
    "420 RETURN",
    "430 *REDRAW",
    "440 CLS 3",
    "450 X = 50",
    "460 Y = 280",
    "470 A = 0",
    "480 L = 486",
    "490 GOSUB *KOCH",
    "500 RETURN",
    "510 *KOCH",
    "520 IF D > 0 THEN GOTO 600",
    "530 GOSUB *DRAW",
    "540 RETURN",
    "600 D = D - 1",
    "610 L = L / 3",
    "620 GOSUB *KOCH",
    "630 A = A + 60",
    "640 GOSUB *KOCH",
    "650 A = A - 120",
    "660 GOSUB *KOCH",
    "670 A = A + 60",
    "680 GOSUB *KOCH",
    "690 D = D + 1",
    "700 L = L * 3",
    "710 RETURN",
    "720 *DRAW",
    "730 RAD = A * 0.01745329",
    "740 X2 = X + L * COS(RAD)",
    "750 Y2 = Y - L * SIN(RAD)",
    "760 LINE (X,Y)-(X2,Y2),CYAN",
    "770 X = X2",
    "780 Y = Y2",
    "790 RETURN",
};
static const char *S_dragon[] = {
    "10 CLS 3",
    "20 X = 200",
    "30 Y = 130",
    "40 A = 0",
    "50 L = 15",
    "60 D = 8",
    "70 GOSUB *DRAGA",
    "80 END",
    "100 *DRAGA",
    "110 IF D > 0 THEN GOTO 200",
    "120 GOSUB *DRAW",
    "130 RETURN",
    "200 D = D - 1",
    "210 GOSUB *DRAGA",
    "220 A = A + 90",
    "230 GOSUB *DRAGB",
    "240 D = D + 1",
    "250 RETURN",
    "300 *DRAGB",
    "310 IF D > 0 THEN GOTO 400",
    "320 GOSUB *DRAW",
    "330 RETURN",
    "400 D = D - 1",
    "410 GOSUB *DRAGA",
    "420 A = A - 90",
    "430 GOSUB *DRAGB",
    "440 D = D + 1",
    "450 RETURN",
    "500 *DRAW",
    "510 RAD = A * 0.01745329",
    "520 X2 = X + L * COS(RAD)",
    "530 Y2 = Y - L * SIN(RAD)",
    "540 LINE (X,Y)-(X2,Y2),CYAN",
    "550 X = X2",
    "560 Y = Y2",
    "570 RETURN"
};
static const char *S_hanoi[] = {
    "10 CLS 3",
    "20 D1=1",
    "30 D2=1",
    "40 D3=1",
    "50 GOSUB *DRAW",
    "60 PRINT \"TOWER OF HANOI - 3 DISKS\"",
    "70 PRINT \"AUTO-ADVANCING EVERY 1 SECOND\"",
    "80 WAIT",
    "90 PRINT \"MOVE 1: A->C (DISK 1)\"",
    "100 D1=3",
    "110 GOSUB *DRAW",
    "120 WAIT",
    "130 PRINT \"MOVE 2: A->B (DISK 2)\"",
    "140 D2=2",
    "150 GOSUB *DRAW",
    "160 WAIT",
    "170 PRINT \"MOVE 3: C->B (DISK 1)\"",
    "180 D1=2",
    "190 GOSUB *DRAW",
    "200 WAIT",
    "210 PRINT \"MOVE 4: A->C (DISK 3)\"",
    "220 D3=3",
    "230 GOSUB *DRAW",
    "240 WAIT",
    "250 PRINT \"MOVE 5: B->A (DISK 1)\"",
    "260 D1=1",
    "270 GOSUB *DRAW",
    "280 WAIT",
    "290 PRINT \"MOVE 6: B->C (DISK 2)\"",
    "300 D2=3",
    "310 GOSUB *DRAW",
    "320 WAIT",
    "330 PRINT \"MOVE 7: A->C (DISK 1)\"",
    "340 D1=3",
    "350 GOSUB *DRAW",
    "360 PRINT \"COMPLETE!\"",
    "370 END",
    "1000 *DRAW",
    "1010 CLS 2",
    "1020 LINE (130,130)-(130,250),WHITE",
    "1030 LINE (330,130)-(330,250),WHITE",
    "1040 LINE (530,130)-(530,250),WHITE",
    "1050 LINE (50,250)-(610,250),WHITE",
    "1060 H1=0",
    "1070 H2=0",
    "1080 IF D2=D1 THEN H1=H1+1",
    "1090 IF D3=D1 THEN H1=H1+1",
    "1100 IF D3=D2 THEN H2=H2+1",
    "1110 X1=130+(D1-1)*200",
    "1120 X2=130+(D2-1)*200",
    "1130 X3=130+(D3-1)*200",
    "1140 Y1=220-H1*30",
    "1150 Y2=220-H2*30",
    "1160 CIRCLE (X1,Y1),10,CYAN",
    "1170 CIRCLE (X2,Y2),20,LIME",
    "1180 CIRCLE (X3,220),30,RED",
    "1190 RETURN"
};
static const char *S_flight[] = {
    "10 CLS 3",
    "20 PRINT \"WIREFRAME FLIGHT: TAKEOFF -> ORBIT -> RETURN -> LAND\"",
    "30 PI = 3.14159265",
    "40 TX = 0",
    "50 TZ = 25",
    "60 TW = 1.5",
    "70 TH = 12",
    "80 DIM TVX(8)",
    "90 DIM TVY(8)",
    "100 DIM TVZ(8)",
    "110 DIM TPX(8)",
    "120 DIM TPY(8)",
    "130 DIM TVK(8)",
    "140 TVX(0) = TX - TW",
    "150 TVY(0) = 0",
    "160 TVZ(0) = TZ - TW",
    "170 TVX(1) = TX + TW",
    "180 TVY(1) = 0",
    "190 TVZ(1) = TZ - TW",
    "200 TVX(2) = TX + TW",
    "210 TVY(2) = 0",
    "220 TVZ(2) = TZ + TW",
    "230 TVX(3) = TX - TW",
    "240 TVY(3) = 0",
    "250 TVZ(3) = TZ + TW",
    "260 TVX(4) = TX - TW",
    "270 TVY(4) = TH",
    "280 TVZ(4) = TZ - TW",
    "290 TVX(5) = TX + TW",
    "300 TVY(5) = TH",
    "310 TVZ(5) = TZ - TW",
    "320 TVX(6) = TX + TW",
    "330 TVY(6) = TH",
    "340 TVZ(6) = TZ + TW",
    "350 TVX(7) = TX - TW",
    "360 TVY(7) = TH",
    "370 TVZ(7) = TZ + TW",
    "371 DIM RGX(6)",
    "372 DIM RGZ(6)",
    "373 DIM RPX(6)",
    "374 DIM RPY(6)",
    "375 DIM RGK(6)",
    "376 FOR I = 0 TO 5",
    "377 RGX(I) = TX + 3 * SIN(I * 2 * PI / 6)",
    "378 RGZ(I) = TZ + 3 * COS(I * 2 * PI / 6)",
    "379 NEXT",
    "380 CX = 0",
    "381 DIM LSX(12)",
    "382 DIM LSY(12)",
    "383 DIM LSK(12)",
    "384 DIM RSX(12)",
    "385 DIM RSY(12)",
    "386 DIM RSK(12)",
    "390 CY = 0",
    "400 CZ = -10",
    "410 HD = 0",
    "415 MODE = 0",
    "416 BUTTON 0, \"VIEW\"",
    "420 T = 0",
    "421 DIM PA(8)",
    "422 PA(0) = -8",
    "423 PA(1) = 8",
    "424 PA(2) = -12",
    "425 PA(3) = 12",
    "426 PA(4) = 8",
    "427 PA(5) = 13",
    "428 PA(6) = 22",
    "429 PA(7) = 32",
    "430 *FRAME",
    "431 IF T >= 560 THEN T = 0",
    "432 MV = BTN(0)",
    "434 IF MV > 0 THEN MODE = 1 - MODE : CLS 3",
    "440 IF T < 70 THEN GOTO *PH1",
    "450 IF T < 130 THEN GOTO *PH2",
    "460 IF T < 330 THEN GOTO *PH3",
    "470 IF T < 380 THEN GOTO *PH4",
    "480 IF T < 440 THEN GOTO *PH5",
    "490 IF T < 560 THEN GOTO *PH6",
    "500 GOTO *DONE",
    "510 *PH1",
    "520 PR = T / 70",
    "530 CY = PR * 8",
    "540 CZ = -10 + PR * 25",
    "550 CX = 0",
    "560 HD = 0",
    "570 GOTO *RENDER",
    "580 *PH2",
    "590 PR = (T - 70) / 60",
    "600 CY = 8",
    "610 CZ = 15 + PR * 2",
    "620 CX = 0",
    "630 HD = 0",
    "640 GOTO *RENDER",
    "650 *PH3",
    "660 PR = (T - 130) / 200",
    "670 PHI = PR * 2 * PI",
    "680 CX = TX + 8 * SIN(PHI)",
    "690 CZ = TZ - 8 * COS(PHI)",
    "700 CY = 8",
    "710 HD = 2 * PI - PHI",
    "720 GOTO *RENDER",
    "730 *PH4",
    "740 PR = (T - 330) / 50",
    "750 CX = TX",
    "760 CY = 8",
    "770 CZ = TZ - 8",
    "780 HD = PR * PI",
    "790 GOTO *RENDER",
    "800 *PH5",
    "810 PR = (T - 380) / 60",
    "820 CX = 0",
    "830 CY = 8",
    "840 CZ = (TZ - 8) - PR * 2",
    "850 HD = PI",
    "860 GOTO *RENDER",
    "870 *PH6",
    "880 PR = (T - 440) / 120",
    "890 CX = 0",
    "900 CY = 8 - PR * 8",
    "910 CZ = 15 - PR * 25",
    "920 IF CY < 0 THEN CY = 0",
    "930 HD = PI",
    "940 GOTO *RENDER",
    "950 *RENDER",
    "960 CHD = COS(HD)",
    "970 SHD = SIN(HD)",
    "975 BA = 0",
    "976 IF T >= 130 THEN IF T < 380 THEN BA = -0.3",
    "977 CBA = COS(BA)",
    "978 SBA = SIN(BA)",
    "979 IF MODE = 1 THEN GOSUB *TAILSET",
    "980 CLS 2",
    "990 FOR K = 0 TO 5",
    "991 TH3 = K * PI / 3 + 0.2",
    "992 WX = 50 * SIN(TH3)",
    "993 WY = 12",
    "994 WZ = 50 * COS(TH3)",
    "995 GOSUB *PROJ",
    "996 IF OK = 1 THEN LINE (SX,SY)-(SX+1,SY),WHITE",
    "997 NEXT",
    "1010 LINE (320 - 320 * CBA, 200 - 320 * SBA)-(320 + 320 * CBA, 200 + 320 * SBA),YELLOW",
    "1020 FOR K = 0 TO 3",
    "1025 WX = PA(K)",
    "1030 WY = 0",
    "1035 WZ = PA(K + 4)",
    "1040 GOSUB *PROJ",
    "1045 PBX = SX",
    "1050 PBY = SY",
    "1055 PBK = OK",
    "1060 WY = 6",
    "1065 GOSUB *PROJ",
    "1070 IF PBK = 1 THEN IF OK = 1 THEN LINE (PBX,PBY)-(SX,SY),GREEN",
    "1075 NEXT",
    "1119 NOROLL = 1",
    "1120 FOR K = 0 TO 11",
    "1125 WY = 0",
    "1130 WZ = -12 + K * 3",
    "1135 WX = 0",
    "1140 GOSUB *PROJ",
    "1145 IF OK = 1 THEN LINE (SX-4,SY)-(SX+4,SY),WHITE",
    "1150 WX = -1",
    "1155 GOSUB *PROJ",
    "1160 LSX(K) = SX",
    "1162 LSY(K) = SY",
    "1164 LSK(K) = OK",
    "1166 WX = 1",
    "1168 GOSUB *PROJ",
    "1170 RSX(K) = SX",
    "1172 RSY(K) = SY",
    "1174 RSK(K) = OK",
    "1180 NEXT",
    "1182 FOR K = 0 TO 10",
    "1183 K2 = K + 1",
    "1184 IF LSK(K) = 1 THEN IF LSK(K2) = 1 THEN LINE (LSX(K),LSY(K))-(LSX(K2),LSY(K2)),WHITE",
    "1185 IF RSK(K) = 1 THEN IF RSK(K2) = 1 THEN LINE (RSX(K),RSY(K))-(RSX(K2),RSY(K2)),WHITE",
    "1186 NEXT",
    "1187 NOROLL = 0",
    "1190 FOR K = 0 TO 5",
    "1200 WX = RGX(K)",
    "1210 WY = 0",
    "1220 WZ = RGZ(K)",
    "1230 GOSUB *PROJ",
    "1240 RPX(K) = SX",
    "1250 RPY(K) = SY",
    "1260 RGK(K) = OK",
    "1270 NEXT",
    "1280 FOR K = 0 TO 5",
    "1290 K2 = (K + 1) MOD 6",
    "1300 IF RGK(K) = 1 THEN IF RGK(K2) = 1 THEN LINE (RPX(K),RPY(K))-(RPX(K2),RPY(K2)),MAGENTA",
    "1310 NEXT",
    "1420 FOR K = 0 TO 7",
    "1430 WX = TVX(K)",
    "1440 WY = TVY(K)",
    "1450 WZ = TVZ(K)",
    "1460 GOSUB *PROJ",
    "1470 TPX(K) = SX",
    "1480 TPY(K) = SY",
    "1490 TVK(K) = OK",
    "1500 NEXT",
    "1510 IF TVK(0) = 1 THEN IF TVK(1) = 1 THEN LINE (TPX(0),TPY(0))-(TPX(1),TPY(1)),CYAN",
    "1520 IF TVK(1) = 1 THEN IF TVK(2) = 1 THEN LINE (TPX(1),TPY(1))-(TPX(2),TPY(2)),CYAN",
    "1530 IF TVK(2) = 1 THEN IF TVK(3) = 1 THEN LINE (TPX(2),TPY(2))-(TPX(3),TPY(3)),CYAN",
    "1540 IF TVK(3) = 1 THEN IF TVK(0) = 1 THEN LINE (TPX(3),TPY(3))-(TPX(0),TPY(0)),CYAN",
    "1550 IF TVK(4) = 1 THEN IF TVK(5) = 1 THEN LINE (TPX(4),TPY(4))-(TPX(5),TPY(5)),CYAN",
    "1560 IF TVK(5) = 1 THEN IF TVK(6) = 1 THEN LINE (TPX(5),TPY(5))-(TPX(6),TPY(6)),CYAN",
    "1570 IF TVK(6) = 1 THEN IF TVK(7) = 1 THEN LINE (TPX(6),TPY(6))-(TPX(7),TPY(7)),CYAN",
    "1580 IF TVK(7) = 1 THEN IF TVK(4) = 1 THEN LINE (TPX(7),TPY(7))-(TPX(4),TPY(4)),CYAN",
    "1590 IF TVK(0) = 1 THEN IF TVK(4) = 1 THEN LINE (TPX(0),TPY(0))-(TPX(4),TPY(4)),CYAN",
    "1600 IF TVK(1) = 1 THEN IF TVK(5) = 1 THEN LINE (TPX(1),TPY(1))-(TPX(5),TPY(5)),CYAN",
    "1610 IF TVK(2) = 1 THEN IF TVK(6) = 1 THEN LINE (TPX(2),TPY(2))-(TPX(6),TPY(6)),CYAN",
    "1620 IF TVK(3) = 1 THEN IF TVK(7) = 1 THEN LINE (TPX(3),TPY(3))-(TPX(7),TPY(7)),CYAN",
    "1630 IF MODE = 0 THEN LINE (320 - 10 * CBA, 200 - 10 * SBA)-(320 + 10 * CBA, 200 + 10 * SBA),RED",
    "1640 IF MODE = 0 THEN LINE (320 + 10 * SBA, 200 - 10 * CBA)-(320 - 10 * SBA, 200 + 10 * CBA),RED",
    "1645 IF MODE = 1 THEN GOSUB *PLANE3D",
    "1650 T = T + 8",
    "1660 WAIT 0.001",
    "1670 GOTO *FRAME",
    "1680 *DONE",
    "1690 PRINT \"FLIGHT COMPLETE\"",
    "1700 END",
    "2000 *PROJ",
    "2010 DX = WX - CX",
    "2020 DY = WY - CY",
    "2030 DZ = WZ - CZ",
    "2040 LX = DX * CHD - DZ * SHD",
    "2050 LZ = DX * SHD + DZ * CHD",
    "2060 LY = DY",
    "2070 OK = 1",
    "2080 IF LZ < 0.5 THEN OK = 0",
    "2090 IF OK = 0 THEN RETURN",
    "2100 SX = 320 + 200 * LX / LZ",
    "2110 SY = 200 - 200 * LY / LZ",
    "2111 IF NOROLL = 1 THEN RETURN",
    "2112 RX = SX - 320",
    "2114 RY = SY - 200",
    "2116 SX = 320 + RX * CBA - RY * SBA",
    "2118 SY = 200 + RX * SBA + RY * CBA",
    "2120 RETURN",
    "2200 *TAILSET",
    "2210 BA = 0 : CBA = 1 : SBA = 0",
    "2220 PCX = CX : PCY = CY : PCZ = CZ",
    "2230 CX = CX - SHD * 6",
    "2240 CZ = CZ - CHD * 6",
    "2250 CY = CY + 2.5",
    "2260 RETURN",
    "2300 *PLANE3D",
    "2305 WX = PCX + SHD * 4 : WY = PCY : WZ = PCZ + CHD * 4",
    "2310 GOSUB *PROJ",
    "2315 NX = SX : NY = SY : NK = OK",
    "2320 WX = PCX - SHD * 3 : WY = PCY : WZ = PCZ - CHD * 3",
    "2325 GOSUB *PROJ",
    "2330 QX = SX : QY = SY : QK = OK",
    "2335 WX = PCX + CHD * 4 : WY = PCY : WZ = PCZ - SHD * 4",
    "2340 GOSUB *PROJ",
    "2345 UX = SX : UY = SY : UK = OK",
    "2350 WX = PCX - CHD * 4 : WY = PCY : WZ = PCZ + SHD * 4",
    "2355 GOSUB *PROJ",
    "2360 VX = SX : VY = SY : VK = OK",
    "2365 WX = PCX - SHD * 3 : WY = PCY + 2 : WZ = PCZ - CHD * 3",
    "2370 GOSUB *PROJ",
    "2375 HX = SX : HY = SY : HK = OK",
    "2380 IF NK = 1 THEN IF QK = 1 THEN LINE (QX,QY)-(NX,NY),WHITE",
    "2385 IF UK = 1 THEN IF VK = 1 THEN LINE (UX,UY)-(VX,VY),CYAN",
    "2390 IF QK = 1 THEN IF HK = 1 THEN LINE (QX,QY)-(HX,HY),WHITE",
    "2395 RETURN",
};
static const char *S_rescue[] = {
    "10 CLS 3",
    "20 PI = 3.14159265",
    "30 H = 5",
    "40 VX0 = 6",
    "50 VZ0 = 5",
    "60 CX = 0",
    "70 CY = 2.5",
    "80 CZ = -6",
    "90 DIM TX(3)",
    "100 DIM TZ(3)",
    "110 TX(0) = 0.5",
    "120 TZ(0) = 0.5",
    "130 TX(1) = 0.5",
    "140 TZ(1) = -0.5",
    "150 TX(2) = -0.5",
    "160 TZ(2) = -0.5",
    "170 TX(3) = -0.5",
    "180 TZ(3) = 0.5",
    "190 DIM TSX(3)",
    "200 DIM TSY(3)",
    "210 DIM TOK(3)",
    "220 T = 0",
    "225 C = 0",
    "230 *FRAME",
    "240 CLS 2",
    "250 IF T < 30 THEN GOTO *PH1",
    "260 IF T < 90 THEN GOTO *PH2",
    "270 IF T < 105 THEN GOTO *PH3",
    "280 IF T < 120 THEN GOTO *PH4",
    "290 IF T < 135 THEN GOTO *PH5",
    "300 IF T < 195 THEN GOTO *PH6",
    "310 IF T < 225 THEN GOTO *PH7",
    "320 GOTO *PH8",
    "330 *PH1",
    "340 DPX = 0",
    "350 DPY = T * H / 30",
    "360 DPZ = 0",
    "370 GOTO *VPHASE",
    "380 *PH2",
    "390 U = (T - 30) / 60",
    "400 DPX = U * VX0",
    "410 DPY = H",
    "420 DPZ = U * VZ0",
    "430 GOTO *VPHASE",
    "440 *PH3",
    "450 U = (T - 90) / 15",
    "460 DPX = VX0",
    "470 DPY = H - U * (H - 1)",
    "480 DPZ = VZ0",
    "490 GOTO *VPHASE",
    "500 *PH4",
    "510 DPX = VX0",
    "520 DPY = 1",
    "530 DPZ = VZ0",
    "540 GOTO *VPHASE",
    "550 *PH5",
    "560 U = (T - 120) / 15",
    "570 DPX = VX0",
    "580 DPY = 1 + U * (H - 1)",
    "590 DPZ = VZ0",
    "600 GOTO *VPHASE",
    "610 *PH6",
    "620 U = (T - 135) / 60",
    "630 DPX = VX0 * (1 - U)",
    "640 DPY = H",
    "650 DPZ = VZ0 * (1 - U)",
    "660 GOTO *VPHASE",
    "670 *PH7",
    "680 U = (T - 195) / 30",
    "690 DPX = 0",
    "700 DPY = H * (1 - U)",
    "710 DPZ = 0",
    "720 GOTO *VPHASE",
    "730 *PH8",
    "740 DPX = 0",
    "750 DPY = 0",
    "760 DPZ = 0",
    "770 *VPHASE",
    "780 IF T < 105 THEN GOTO *VP1",
    "790 IF T < 120 THEN GOTO *VP2",
    "800 GOTO *VP3",
    "810 *VP1",
    "820 VPX = VX0",
    "830 VPY = 0",
    "840 VPZ = VZ0",
    "850 GOTO *VPEND",
    "860 *VP2",
    "870 U = (T - 105) / 15",
    "880 VPX = VX0",
    "890 VPY = U",
    "900 VPZ = VZ0",
    "910 GOTO *VPEND",
    "920 *VP3",
    "930 VPX = DPX",
    "940 VPY = DPY - 0.7",
    "950 VPZ = DPZ",
    "955 IF VPY < 0 THEN VPY = 0",
    "960 *VPEND",
    "970 VAR = 0.5 + 0.4 * SIN(T * 0.5)",
    "980 SVAR = SIN(VAR)",
    "990 CVAR = COS(VAR)",
    "1000 FOR I = 0 TO 2",
    "1010 ZL = I * 4",
    "1020 WX = -5",
    "1030 WY = 0",
    "1040 WZ = ZL",
    "1050 GOSUB *PROJ",
    "1060 GSX = SX",
    "1070 GSY = SY",
    "1080 GOK = OK",
    "1090 WX = 5",
    "1100 GOSUB *PROJ",
    "1110 IF GOK = 1 THEN IF OK = 1 THEN LINE (GSX,GSY)-(SX,SY),BLUE",
    "1120 NEXT",
    "1130 FOR I = 0 TO 2",
    "1140 XL = -5 + I * 5",
    "1150 WX = XL",
    "1160 WY = 0",
    "1170 WZ = -2",
    "1180 GOSUB *PROJ",
    "1190 GSX = SX",
    "1200 GSY = SY",
    "1210 GOK = OK",
    "1220 WZ = 10",
    "1230 GOSUB *PROJ",
    "1240 IF GOK = 1 THEN IF OK = 1 THEN LINE (GSX,GSY)-(SX,SY),BLUE",
    "1250 NEXT",
    "1260 WX = VPX",
    "1270 WY = VPY",
    "1280 WZ = VPZ",
    "1290 GOSUB *PROJ",
    "1300 SBSX = SX",
    "1310 SBSY = SY",
    "1320 SBOK = OK",
    "1330 WY = VPY + 1.4",
    "1340 GOSUB *PROJ",
    "1350 SHSX = SX",
    "1360 SHSY = SY",
    "1370 SHOK = OK",
    "1380 WY = VPY + 1.7",
    "1390 GOSUB *PROJ",
    "1400 IF SBOK = 1 THEN IF SHOK = 1 THEN LINE (SBSX,SBSY)-(SHSX,SHSY),GREEN",
    "1405 HR = 75 / (VPZ - CZ)",
    "1410 IF OK = 1 THEN CIRCLE (SX,SY),HR,GREEN",
    "1420 WX = VPX - SVAR * 0.5",
    "1430 WY = VPY + 1.4 + CVAR * 0.5",
    "1440 WZ = VPZ",
    "1450 GOSUB *PROJ",
    "1460 IF SHOK = 1 THEN IF OK = 1 THEN LINE (SHSX,SHSY)-(SX,SY),GREEN",
    "1470 WX = VPX + SVAR * 0.5",
    "1480 GOSUB *PROJ",
    "1490 IF SHOK = 1 THEN IF OK = 1 THEN LINE (SHSX,SHSY)-(SX,SY),GREEN",
    "1500 WX = DPX",
    "1510 WY = DPY",
    "1520 WZ = DPZ",
    "1530 GOSUB *PROJ",
    "1540 DCSX = SX",
    "1550 DCSY = SY",
    "1560 DCOK = OK",
    "1570 FOR I = 0 TO 3",
    "1580 WX = DPX + TX(I)",
    "1590 WY = DPY",
    "1600 WZ = DPZ + TZ(I)",
    "1610 GOSUB *PROJ",
    "1620 TSX(I) = SX",
    "1630 TSY(I) = SY",
    "1640 TOK(I) = OK",
    "1650 NEXT",
    "1660 FOR I = 0 TO 3",
    "1670 IF DCOK = 1 THEN IF TOK(I) = 1 THEN LINE (DCSX,DCSY)-(TSX(I),TSY(I)),YELLOW",
    "1680 NEXT",
    "1690 DR = 60 / (DPZ - CZ)",
    "1700 FOR I = 0 TO 3",
    "1710 IF TOK(I) = 1 THEN CIRCLE (TSX(I),TSY(I)),DR,CYAN",
    "1720 NEXT",
    "1730 T = T + 5",
    "1740 IF T < 240 THEN GOTO 1750",
    "1742 C = C + 1",
    "1744 T = 0",
    "1746 IF C >= 3 THEN GOTO 1765",
    "1750 WAIT 0.01",
    "1760 GOTO *FRAME",
    "1765 CLS 3",
    "1766 PRINT \"RESCUE COMPLETE - 3 SAVED\"",
    "1767 END",
    "1770 *PROJ",
    "1780 DX = WX - CX",
    "1790 DY = WY - CY",
    "1800 DZ = WZ - CZ",
    "1810 OK = 1",
    "1820 IF DZ < 0.5 THEN OK = 0",
    "1830 IF OK = 0 THEN RETURN",
    "1840 SX = 320 + 300 * DX / DZ",
    "1850 SY = 200 - 300 * DY / DZ",
    "1860 RETURN"
};

static const struct { const char *name; const char *const *line; int n; } samples[] = {
    { "hello.bas",   S_hello,   2 },
    { "forloop.bas", S_forloop, 3 },
    { "mtable.bas",  S_mtable,  4 },
    { "add.bas",     S_add,     4 },
    { "sum100.bas",  S_sum100,  5 },
    { "fibon.bas",   S_fibon,   8 },
    { "squares.bas", S_squares, 4 },
    { "rotate.bas",  S_rotate,  12 },
    { "strings.bas", S_strings, 7 },
    { "bsort.bas",   S_bsort,   31 },
    { "fizz.bas",    S_fizz,    6 },
    { "table.bas",   S_table,   5 },
    { "count.bas",   S_count,   6 },
    { "koch.bas",     S_koch,     (int)(sizeof(S_koch)/sizeof(S_koch[0])) },
    { "dragon.bas",   S_dragon,   36 },
    { "bubble.bas",   S_bubble,   32 },
    { "qsort.bas",    S_qsort,    60 },
    { "hanoi.bas",    S_hanoi,    57 },
    { "maze.bas",     S_maze,     180 },
    { "glass.bas",    S_glass,    (int)(sizeof(S_glass)/sizeof(S_glass[0])) },
    { "flight.bas",   S_flight,   (int)(sizeof(S_flight)/sizeof(S_flight[0])) },
    { "rescue.bas",   S_rescue,   (int)(sizeof(S_rescue)/sizeof(S_rescue[0])) },
};
#define NSAMPLE ((int)(sizeof(samples) / sizeof(samples[0])))

/* case-insensitive string equality */
static int b_streqi(const char *a, const char *b)
{
    while (*a && *b) { if (b_up(*a) != b_up(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* Normalise a raw filename into nm[] (lower-cased, ".bas" appended when no
 * extension was given) so RUN "HELLO" matches the stored "hello.bas". */
static void norm_name(const char *raw, char *nm, int max)
{
    int i = 0, dot = 0;
    while (raw[i] && i < max - 5) {
        char c = raw[i]; if (c >= 'A' && c <= 'Z') c += 32;
        if (c == '.') dot = 1;
        nm[i] = c; i++;
    }
    nm[i] = 0;
    if (!dot) { nm[i++] = '.'; nm[i++] = 'b'; nm[i++] = 'a'; nm[i++] = 's'; nm[i] = 0; }
}

/* FILES — list the saved (sample) programs, one per line. */
static void do_files(void)
{
    /* Lay the names out in columns across the window width (60-char grid):
     * 4 columns of 15 instead of one name per line. */
    const int colw = 15, ncol = 4;
    int col = 0;
    for (int s = 0; s < NSAMPLE; s++) {
        const char *nm = samples[s].name;
        int len = 0; while (nm[len]) len++;
        emit(nm);
        if (++col >= ncol) { emit("\n"); col = 0; }
        else { for (int p = len; p < colw; p++) emitc(' '); }
    }
    if (col != 0) emit("\n");
}

/* Load a named sample into the program buffer; returns 1 on success. */
static int load_named(const char *raw)
{
    char nm[40]; norm_name(raw, nm, sizeof nm);
    for (int s = 0; s < NSAMPLE; s++) {
        if (b_streqi(samples[s].name, nm)) {
            nprog = 0;
            var_clear_all();
            for (int j = 0; j < samples[s].n; j++) {
                const char *p = samples[s].line[j];
                int no = 0; while (*p >= '0' && *p <= '9') no = no * 10 + (*p++ - '0');
                while (*p == ' ') p++;
                prog_set(no, p);
            }
            return 1;
        }
    }
    return 0;
}

/* Parse a quoted "name" starting at ip into buf; returns 1 if present. */
static int parse_quoted(char *buf, int max)
{
    skipsp();
    if (*ip != '"') return 0;
    ip++;
    int n = 0; while (*ip && *ip != '"' && n < max - 1) buf[n++] = *ip++;
    buf[n] = 0;
    if (*ip == '"') ip++;
    return 1;
}

/* ---- statements ---------------------------------------------------- */
static void do_print(void)
{
    int trailing = 0;
    for (;;) {
        skipsp();
        if (*ip == 0 || *ip == ':') break;
        if (peek_is_string()) { char s[SVAR_LEN]; seval(s, sizeof s); emit(s); }
        else { double v = expr(); char nb[40]; num_str(v, nb); emit(nb); }
        skipsp(); trailing = 0;
        if (*ip == ';') { ip++; trailing = 1; }
        else if (*ip == ',') { ip++; emit("\t"); trailing = 1; }
        else break;
    }
    if (!trailing) emit("\n");
}
static void do_input(void)
{
    /* Capture the prompt we emit (optional "literal" + "? ") so we can strip
     * it back off: the windowed full-screen editor returns the whole on-screen
     * line — prompt included — exactly like the `debug` prompt does. */
    char prompt[80]; int pl = 0;
    skipsp();
    if (*ip == '"') {
        ip++;
        while (*ip && *ip != '"') { if (pl < (int)sizeof prompt - 3) prompt[pl++] = *ip; ip++; }
        if (*ip == '"') ip++;
        skipsp(); if (*ip == ';' || *ip == ',') ip++;
    }
    prompt[pl++] = '?'; prompt[pl++] = ' '; prompt[pl] = 0;
    emit(prompt);
    char inbuf[96]; int n = g_input ? g_input(inbuf, sizeof inbuf) : -1;
    if (n < 0) { berr("no input"); return; }
    const char *src = inbuf;
    /* strip the echoed prompt prefix if the editor returned it */
    { int i = 0; while (prompt[i] && src[i] == prompt[i]) i++; if (prompt[i] == 0) src += i; }
    for (;;) {
        const char *save = ip; int si = svaridx();
        if (si >= 0) { while (*src == ' ' || *src == ',') src++;
            int n = 0; while (*src && *src != ',' && n < SVAR_LEN - 1) vstr[si][n++] = *src++;
            while (n > 0 && vstr[si][n-1] == ' ') n--; vstr[si][n] = 0;
            skipsp(); if (*ip == ',') { ip++; continue; } break; }
        ip = save;
        int vi = varidx(); if (vi < 0) { berr("INPUT var"); return; }
        while (*src == ' ' || *src == ',') src++;
        int neg = 0; double v = 0, f;
        if (*src == '-') { neg = 1; src++; }
        while (b_isdigit(*src)) v = v * 10 + (*src++ - '0');
        if (*src == '.') { src++; f = 0.1; while (b_isdigit(*src)) { v += (*src++ - '0') * f; f *= 0.1; } }
        vval[vi] = neg ? -v : v;
        skipsp(); if (*ip == ',') { ip++; continue; } break;
    }
}

static void exec_stmt(void)
{
    skipsp();
    while (*ip == ':') { ip++; skipsp(); }   /* tolerate a resumed-at ':' */
    if (*ip == 0) return;
    if (*ip == '*') { while (*ip) ip++; return; }   /* a label line — no-op */
    if (kw("REM")) { while (*ip) ip++; return; }
    if (*ip == '?') { ip++; do_print(); return; }
    if (kw("PRINT")) { do_print(); return; }
    if (kw("LIST")) { do_list(); return; }
    if (kw("NEW"))  { nprog = 0; var_clear_all();
                      whiletop = 0; data_pc = -1; data_ip = 0; emit("Ok\n"); return; }
    if (kw("END") || kw("STOP")) { running = 0; return; }
    if (kw("GOTO"))  { g_goto = goto_target(); return; }
    if (kw("GOSUB")) { int tgt = goto_target();
        if (gosubtop < GOSUB_MAX) { gosubstk[gosubtop].rpc = pc; gosubstk[gosubtop].rip = ip; gosubtop++; }
        else berr("GOSUB overflow");
        g_goto = tgt; return; }
    if (kw("RETURN")){ if (gosubtop > 0) { pc = gosubstk[--gosubtop].rpc; ip = gosubstk[gosubtop].rip; g_goto = -2; }
                       else berr("RETURN without GOSUB"); return; }
    if (kw("WIFI")) {                         /* WIFI ON | OFF | STATUS */
        int action = 2; skipsp();
        if (kw("ON")) action = 1;
        else if (kw("OFF")) action = 0;
        else if (kw("STATUS")) action = 2;
        if (g_wifi) g_wifi(action);
        return;
    }
    if (kw("INPUT")) { do_input(); return; }
    if (kw("IF")) {
        double c = expr(); skipsp();
        if (!kw("THEN")) { berr("expected THEN"); return; }
        skipsp();
        if (c != 0) { if (b_isdigit(*ip)) g_goto = (int)expr(); else exec_stmt(); }
        else while (*ip) ip++;
        return;
    }
    if (kw("FOR")) {
        int vi = varidx(); skipsp();
        if (*ip != '=') { berr("FOR ="); return; } ip++;
        double start = expr(); if (!kw("TO")) { berr("FOR TO"); return; }
        double limit = expr(); double step = 1; if (kw("STEP")) step = expr();
        vval[vi] = start;
        if (fortop < FOR_MAX) { forstk[fortop].var = vi; forstk[fortop].limit = limit; forstk[fortop].step = step;
                                forstk[fortop].idx = pc; forstk[fortop].stmt = ip; fortop++; }
        else berr("FOR overflow");
        return;
    }
    if (kw("NEXT")) {
        const char *save = ip; if (varidx() < 0) ip = save;     /* optional var */
        if (fortop == 0) { berr("NEXT without FOR"); return; }
        int t = fortop - 1;
        vval[forstk[t].var] += forstk[t].step;
        double v = vval[forstk[t].var];
        int again = (forstk[t].step >= 0) ? (v <= forstk[t].limit) : (v >= forstk[t].limit);
        if (again) { pc = forstk[t].idx; ip = forstk[t].stmt; g_goto = -2; } else fortop--;
        return;
    }
    if (kw("CLS")) {                          /* CLS=text  CLS 2=gfx  CLS 3=both */
        int mode = 1; skipsp();
        if (b_isdigit(*ip)) mode = (int)expr();
        if (g_cls) g_cls(mode);
        return;
    }
    if (kw("BUTTON")) {                       /* BUTTON n,"label"[,line] — GUI button */
        int n = (int)expr(); skipsp();
        if (*ip != ',') { berr("BUTTON ,"); return; } ip++;
        char lbl[SVAR_LEN]; seval(lbl, sizeof lbl);
        if (g_button) g_button(n, lbl);
        skipsp();
        if (n >= 0 && n < BTN_MAX) {
            if (*ip == ',') { ip++; g_btn_handler[n] = (int)expr();  /* event handler line */
                              if (g_btn_handler[n] > 0) g_has_btn_handlers = 1; }
            else g_btn_handler[n] = 0;        /* polled: read with BTN(n) */
        }
        return;
    }
    if (kw("PLOT")) {                         /* PLOT x,y[,charcode] */
        int x = (int)expr(); skipsp();
        if (*ip != ',') { berr("PLOT ,"); return; } ip++;
        int y = (int)expr();
        int ch = '*'; skipsp();
        if (*ip == ',') { ip++; ch = (int)expr(); }
        if (g_plot) g_plot(x, y, ch);
        return;
    }
    if (kw("LINE")) {                         /* LINE(x1,y1)-(x2,y2)[,color] */
        int x1, y1, x2, y2, c = 7;
        skipsp(); if (*ip != '(') { berr("LINE ("); return; } ip++;
        x1 = (int)expr(); skipsp(); if (*ip != ',') { berr("LINE ,"); return; } ip++;
        y1 = (int)expr(); skipsp(); if (*ip != ')') { berr("LINE )"); return; } ip++;
        skipsp(); if (*ip != '-') { berr("LINE -"); return; } ip++;
        skipsp(); if (*ip != '(') { berr("LINE ("); return; } ip++;
        x2 = (int)expr(); skipsp(); if (*ip != ',') { berr("LINE ,"); return; } ip++;
        y2 = (int)expr(); skipsp(); if (*ip != ')') { berr("LINE )"); return; } ip++;
        skipsp(); if (*ip == ',') { ip++; c = (int)expr(); }
        if (g_line) g_line(x1, y1, x2, y2, c);
        return;
    }
    if (kw("PAUSE")) { int ms = (int)expr(); if (g_pause) g_pause(ms); return; }
    if (kw("WAIT")) {                         /* WAIT [seconds]; bare = ~1s */
        double sec = 0; skipsp();
        if (b_isdigit(*ip) || *ip == '.') sec = expr();
        int ms = (int)(sec * 1000); if (ms <= 0) ms = 1000;
        if (g_pause) g_pause(ms); return;
    }
    if (kw("CIRCLE")) {                        /* CIRCLE (x,y),r[,color] */
        int cx, cy, r, c = 7;
        skipsp(); if (*ip != '(') { berr("CIRCLE ("); return; } ip++;
        cx = (int)expr(); skipsp(); if (*ip != ',') { berr("CIRCLE ,"); return; } ip++;
        cy = (int)expr(); skipsp(); if (*ip != ')') { berr("CIRCLE )"); return; } ip++;
        skipsp(); if (*ip != ',') { berr("CIRCLE r"); return; } ip++;
        r = (int)expr(); skipsp();
        if (*ip == ',') { ip++; c = (int)expr(); }
        if (g_circle) g_circle(cx, cy, r, c);
        return;
    }
    if (kw("DIM")) {
        for (;;) { char nm[12]; if (var_name(nm) == 0) { berr("DIM var"); return; }
            int li = var_slot(nm); skipsp();
            if (*ip != '(') { berr("DIM ("); return; } ip++;
            int d1 = (int)expr() + 1, d2 = 1, nd = 1; skipsp();
            if (*ip == ',') { ip++; d2 = (int)expr() + 1; nd = 2; skipsp(); }
            if (*ip != ')') { berr("DIM )"); return; } ip++;
            int sz = d1 * d2;
            if (li < 0 || d1 < 1 || d2 < 1 || arrtop + sz > ARR_POOL) { berr("array space"); return; }
            varr[li].base = arrtop; varr[li].d1 = d1; varr[li].d2 = d2; varr[li].ndim = nd;
            for (int k = 0; k < sz; k++) arrpool[arrtop + k] = 0; arrtop += sz;
            skipsp(); if (*ip == ',') { ip++; continue; } break; }
        return;
    }
    if (kw("DATA")) { while (*ip) ip++; return; }        /* inert when reached */
    if (kw("RESTORE")) { data_pc = -1; data_ip = 0; skipsp();
        if (b_isdigit(*ip)) { int idx = find_line((int)expr()); if (idx >= 0) data_pc = idx - 1; }
        return;
    }
    if (kw("READ")) {
        for (;;) { char tok[SVAR_LEN]; const char *save = ip; int si = svaridx();
            if (si >= 0) { if (!data_next(tok, sizeof tok)) { berr("out of DATA"); return; } scopy(vstr[si], SVAR_LEN, tok); }
            else { char nm[12]; ip = save;
                if (var_name(nm) == 0) { berr("READ var"); return; }
                int vi = var_slot(nm); double *e = 0; int isarr = 0; skipsp();
                if (*ip == '(') { e = arr_elem(vi); isarr = 1; }
                if (!data_next(tok, sizeof tok)) { berr("out of DATA"); return; }
                if (isarr) { if (e) *e = str_to_num(tok); } else vval[vi] = str_to_num(tok); }
            skipsp(); if (*ip == ',') { ip++; continue; } break; }
        return;
    }
    if (kw("WHILE")) {
        const char *cond_ip = ip; int cond_pc = pc;
        double c = expr();
        if (c != 0) {
            if (whiletop < WHILE_MAX) { whilestk[whiletop].rpc = cond_pc; whilestk[whiletop].rip = cond_ip; whiletop++; }
            else berr("WHILE overflow");
        } else {                                          /* skip to matching WEND */
            int depth = 1;
            for (int i = pc + 1; i < nprog; i++) {
                const char *t = prog[i].text; while (*t == ' ' || *t == '\t') t++;
                if (b_streqi_kw(t, "WHILE")) depth++;
                else if (b_streqi_kw(t, "WEND")) { if (--depth == 0) { pc = i; ip = prog[i].text;
                    while (*ip) ip++; g_goto = -2; return; } }
            }
            berr("WHILE without WEND");
        }
        return;
    }
    if (kw("WEND")) {
        if (whiletop == 0) { berr("WEND without WHILE"); return; }
        int t = whiletop - 1; const char *after_ip = ip; int after_pc = pc;
        ip = whilestk[t].rip; pc = whilestk[t].rpc;
        double c = expr();
        if (c != 0) { g_goto = -2; }                      /* loop: resume body after cond */
        else { whiletop--; ip = after_ip; pc = after_pc; }/* exit: continue after WEND */
        return;
    }
    (void)kw("LET");
    { const char *save = ip; int si = svaridx(); skipsp();           /* A$ = strexpr */
      if (si >= 0 && *ip == '=') { ip++; seval(vstr[si], SVAR_LEN); return; }
      ip = save; }
    { const char *save = ip; char nm[12]; int n = var_name(nm);     /* V=expr / A(i)=expr */
      if (n > 0 && nm[n - 1] != '$') {
          int vi = var_slot(nm); skipsp();
          if (*ip == '(') {                                          /* array element */
              double *e = arr_elem(vi); skipsp();
              if (e && *ip == '=') { ip++; *e = expr(); return; }
          } else if (*ip == '=') {                                   /* scalar */
              ip++; vval[vi] = expr(); return;
          }
      }
      ip = save; }
    berr("syntax error");
}

/* ---- line debugger (the `debug` command) -------------------------------
 * `debug` runs the loaded program under the debugger, stopping before the
 * first line.  At each stop a `dbg> ` prompt reads one command from the same
 * input path as INPUT (the BASIC window's line queue):
 *   s / <enter>  step one line        c   continue (run to a breakpoint)
 *   b N          set breakpoint @N     b   list breakpoints
 *   d N          clear breakpoint @N   d   clear all breakpoints
 *   p <expr>     evaluate + print      l   list the program
 *   q            stop the program      h   help
 */
static int dbg_atoi(const char *s, int *ok) {
    int v = 0, any = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (ok) *ok = any;
    return v;
}
static int dbg_is_bp(int lineno) {
    int i;
    for (i = 0; i < dbg_nbp; i++) if (dbg_bp[i] == lineno) return 1;
    return 0;
}
static void dbg_show_line(void) {
    char nb[16];
    emit("["); num_str((double)prog[pc].no, nb); emit(nb); emit("] ");
    emit(prog[pc].text); emit("\n");
}
/* Evaluate an expression string (numeric or string) against the live program
 * state and print it.  Saves/restores the interpreter cursor so inspecting a
 * value never disturbs the paused program. */
static void dbg_print_expr(const char *arg) {
    const char *save_ip = ip;
    int save_err = err, save_goto = g_goto;
    ip = arg; err = 0;
    skipsp();
    if (*ip == 0) { emit("?usage: p <expr>\n"); }
    else if (peek_is_string()) {
        char s[SVAR_LEN]; seval(s, sizeof s);
        if (err) emit("?eval error\n"); else { emit("= \""); emit(s); emit("\"\n"); }
    } else {
        double v = expr(); char nb[32];
        if (err) emit("?eval error\n"); else { num_str(v, nb); emit("= "); emit(nb); emit("\n"); }
    }
    ip = save_ip; err = save_err; g_goto = save_goto;
}
static void dbg_bp_add(int lineno) {
    char nb[16];
    if (dbg_is_bp(lineno)) { emit("bp already set\n"); return; }
    if (dbg_nbp >= 16) { emit("?too many breakpoints\n"); return; }
    dbg_bp[dbg_nbp++] = lineno;
    emit("bp set @"); num_str((double)lineno, nb); emit(nb); emit("\n");
}
static void dbg_bp_del(int lineno) {
    int i, j;
    for (i = 0; i < dbg_nbp; i++) if (dbg_bp[i] == lineno) {
        for (j = i; j + 1 < dbg_nbp; j++) dbg_bp[j] = dbg_bp[j + 1];
        dbg_nbp--; emit("bp cleared\n"); return;
    }
    emit("no such bp\n");
}
static void dbg_bp_list(void) {
    int i; char nb[16];
    if (dbg_nbp == 0) { emit("no breakpoints\n"); return; }
    emit("breakpoints:");
    for (i = 0; i < dbg_nbp; i++) { emit(" "); num_str((double)dbg_bp[i], nb); emit(nb); }
    emit("\n");
}
/* Interactive prompt at a stop point.  Returns when the user resumes (step or
 * continue); may set running = 0 to abort the program (`q`). */
static void dbg_repl(void) {
    char cmd[128];
    for (;;) {
        emit("dbg> ");
        int n = g_input ? g_input(cmd, sizeof cmd) : -1;
        if (n < 0) { dbg_on = 0; dbg_step = 0; return; }    /* input closed: detach */
        {
            const char *c = cmd;
            char k;
            /* The windowed full-screen editor returns the whole on-screen line,
             * which includes the "dbg> " prompt we just printed; skip it. */
            { const char *pfx = "dbg> "; int i = 0;
              while (pfx[i] && c[i] == pfx[i]) i++;
              if (pfx[i] == 0) c += i; }
            while (*c == ' ' || *c == '\t') c++;
            k = b_up(*c);
            if (k == 0 || k == 'S' || k == 'N') { dbg_step = 1; return; }   /* step */
            if (k == 'C') { dbg_step = 0; return; }                          /* continue */
            if (k == 'Q') { running = 0; dbg_on = 0; dbg_step = 0; emit("[debug] stopped\n"); return; }
            if (k == 'P') { dbg_print_expr(c + 1); continue; }              /* print expr */
            if (k == 'L') { do_list(); continue; }
            if (k == 'H' || k == '?') {
                emit("dbg: s step  c cont  b N bp  d N del  p expr  l list  q quit\n");
                continue;
            }
            if (k == 'B') { int ok; int ln = dbg_atoi(c + 1, &ok); if (ok) dbg_bp_add(ln); else dbg_bp_list(); continue; }
            if (k == 'D') { int ok; int ln = dbg_atoi(c + 1, &ok); if (ok) dbg_bp_del(ln); else { dbg_nbp = 0; emit("all bp cleared\n"); } continue; }
            emit("?dbg cmd — h for help\n");
        }
    }
}

/* ---- RUN ----------------------------------------------------------- */
static void do_run(void)
{
    running = 1; fortop = 0; gosubtop = 0; whiletop = 0; err = 0; g_break = 0;
    data_pc = -1; data_ip = 0;                 /* rewind DATA          */
    var_clear_all();                                              /* clear vars/arrays */
    if (g_buttons_reset) g_buttons_reset();                       /* drop program buttons */
    { int i; for (i = 0; i < BTN_MAX; i++) { g_btn_handler[i] = 0; g_btn_queue[i] = 0; }
      g_has_btn_handlers = 0; }
    if (nprog == 0) { running = 0; emit("Ok\n"); return; }
    pc = 0; ip = prog[0].text;
    long guard = 0;
    while (running) {
        if (g_break) { g_break = 0; emit("\nBreak\n"); break; }   /* Ctrl-C */
        if (pc < 0 || pc >= nprog) break;
        if (++guard > 8000000L) { emit("\n?runaway stopped\n"); break; }
        /* Poll the keyboard for a mid-run Ctrl-C (the pump is blocked while we
         * run).  Throttled — a USB control transfer per ~32k steps is cheap. */
        if ((guard & 0x7fff) == 0 && g_break_poll && g_break_poll()) {
            emit("\nBreak\n"); break;
        }

        /* Debugger stop point: only at the start of a line (ip == line text),
         * so :-separated statements run as one step.  Stop when single-stepping
         * or when this line carries a breakpoint. */
        if (dbg_on && ip == prog[pc].text &&
            (dbg_step || dbg_is_bp(prog[pc].no))) {
            dbg_show_line();
            dbg_repl();
            if (!running) break;              /* `q` aborted the program */
        }

        /* Event-driven buttons: at the start of a top-level line (not inside
         * the program's own GOSUB), if a BUTTON with a handler was clicked,
         * GOSUB its handler (it RETURNs back here).  gosubtop==0 keeps handlers
         * atomic — they never interrupt the program mid-subroutine. */
        if (g_has_btn_handlers && gosubtop == 0 && ip == prog[pc].text && !dbg_on) {
            int hn = bas_next_btn_event();
            if (hn > 0) {
                int idx = find_line(hn);
                if (idx >= 0) {
                    if (gosubtop < GOSUB_MAX) {
                        gosubstk[gosubtop].rpc = pc; gosubstk[gosubtop].rip = ip; gosubtop++;
                    }
                    pc = idx; ip = prog[pc].text; continue;
                }
            }
        }

        g_goto = -1;
        exec_stmt();                          /* runs at the current ip */

        if (err) {
            char nb[16]; emit("?"); emit(errmsg); emit(" in ");
            num_str((double)prog[pc].no, nb); emit(nb); emit("\n");
            break;
        }
        if (g_goto >= 0) {                    /* GOTO / GOSUB / IF->lineno */
            int idx = find_line(g_goto);
            if (idx < 0) { emit("?undef'd line "); { char nb[16]; num_str((double)g_goto, nb); emit(nb); } emit("\n"); break; }
            pc = idx; ip = prog[pc].text; continue;
        }
        if (g_goto == -2) continue;           /* RETURN / NEXT set pc+ip — resume there */

        /* g_goto == -1: statement finished normally */
        skipsp();
        if (*ip == ':') { ip++; continue; }   /* next statement on the same line */
        pc++;                                 /* advance to the next line */
        if (pc < nprog) ip = prog[pc].text;
    }
    running = 0;
    /* Keep a drawn picture on screen: emitting "Ok" would print text, and the
     * windowed BASIC wipes the graphics layer on the first text after drawing.
     * So skip the "Ok" when the program left the window in graphics mode. */
    if (!err && !(g_gfx_active && g_gfx_active())) emit("Ok\n");
}

/* ---- public: process one typed line ------------------------------- */
void basic_exec_line(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == 0) return;
    if (b_isdigit(*line)) {                   /* program line */
        int no = 0; while (b_isdigit(*line)) no = no * 10 + (*line++ - '0');
        while (*line == ' ') line++;
        prog_set(no, line);
        return;
    }
    err = 0; ip = line;
    if (kw("FILES")) { do_files(); emit("Ok\n"); return; }
    if (kw("LOAD")) {
        char fn[40];
        if (parse_quoted(fn, sizeof fn) && load_named(fn)) emit("Ok\n");
        else emit("?file not found\n");
        return;
    }
    if (kw("RUN")) {
        char fn[40]; const char *save = ip;
        if (parse_quoted(fn, sizeof fn)) {
            if (!load_named(fn)) { emit("?file not found\n"); return; }
        } else { ip = save; }
        do_run(); return;
    }
    if (kw("DEBUG")) {                         /* run under the line debugger */
        char fn[40]; const char *save = ip;
        if (parse_quoted(fn, sizeof fn)) {
            if (!load_named(fn)) { emit("?file not found\n"); return; }
        } else { ip = save; }
        if (nprog == 0) { emit("?no program — type some lines first\n"); return; }
        if (g_fullscreen) g_fullscreen(1);     /* debugger takes the full screen */
        emit("[debug] s step  c cont  b N bp  p expr  q quit  (h help)\n");
        dbg_on = 1; dbg_step = 1;              /* stop before the first line */
        do_run();
        dbg_on = 0; dbg_step = 0;
        if (g_fullscreen) g_fullscreen(0);     /* restore the window size        */
        return;
    }
    /* immediate statement(s) */
    for (;;) {
        g_goto = -1; exec_stmt();
        if (err) { emit("?"); emit(errmsg); emit("\n"); return; }
        skipsp();
        if (*ip == ':') { ip++; continue; }
        break;
    }
    emit("Ok\n");
}

void basic_init(void) { nprog = 0; var_clear_all();
    whiletop = 0; data_pc = -1; data_ip = 0; running = 0; }

#ifdef BASIC_HOST_TEST
static void host_emit(const char *s) { fputs(s, stdout); }
static int  host_input(char *buf, int max) {
    if (!fgets(buf, max, stdin)) return -1;
    int n = (int)strlen(buf); while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0; return n;
}
#ifndef BASIC_NO_MAIN
int main(void)
{
    basic_set_emit(host_emit); basic_set_input(host_input); basic_init();
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        int n = (int)strlen(line); while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        basic_exec_line(line);
    }
    return 0;
}
#endif /* !BASIC_NO_MAIN */
#endif
