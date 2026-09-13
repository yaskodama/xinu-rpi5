/* system/html.c —— 機内ブラウザの HTML/CSS 整形と描画
 *
 * 一回通しの走査で HTML を読み、「描画命令」(op: 座標・倍率・様式・色・文字列) を
 * 積む。描画は op を並べるだけ。表示装置が無い構成でも html_text() で同じ op を
 * 文字に落として /browse から確かめられる（画面を見ずに文字で検算するため）。
 *
 *  ・ブロック（見出し・段落・箇条書き・表の行・区切り線・囲み）を縦に積む
 *  ・行内（強調・リンク・コード・ボタン）を色と太さで描き分ける
 *  ・単語（英語）／一文字（日本語）の境界で折り返す。行頭禁則は閉じ括弧類だけ
 *  ・CSS を読む: <style> と外部 CSS（呼び手が html_set_css で渡す）。
 *    選択子は tag / .class / #id の複合＋子孫（空白）・子（>）。擬似クラスの規則は捨てる。
 *    性質は color / background / border / font-size / font-weight / text-decoration /
 *    text-align / display / margin / padding / list-style / text-transform。
 *    var(--x) と rgba() と clamp() を解く。線形グラデーションは最初の色を採る。
 *    継承するのは color・font-weight・text-align・text-decoration・list-style・text-transform。
 *
 * 文字は 16 ドット（device/video/jpfont.c: ASCII 8x16, JIS X 0208 16x16）。
 * フォントに無い記号（① — “ ” など）は近い ASCII に写し、それも無ければ '.'。
 *
 * ★ 入力は外から来る（サイトの HTML と CSS）。境界検査を落とすと板ごと死ぬので、
 *    すべての添字は cap に対して検査し、Mac 側のハーネス（tools/html_harness.c）で
 *    ASan＋変異入力を掛けてから焼く。 */

#define HT_MAXOPS   8000
#define HT_POOL     98304
#define HT_STACK    64
#define HT_LINKS    256
#define HT_RULES    400
#define HT_VARS     48
#define HT_CSS      32768
#define FH          16          /* 文字の高さ */
#define AW          8           /* ASCII の幅 */
#define JW          16          /* 全角の幅 */
#define LINE_GAP    3
#define GAP_MAX     24          /* 余白の上限（サイトの 4.5rem をそのまま取ると画面が空く） */

/* 様式ビット */
#define ST_BOLD   1
#define ST_ULINE  2
#define ST_CODE   4
#define ST_BTN    8
#define ST_UPPER  16
#define ST_NOWRAP 32

#define C_FG     0xFFE8EEF8U
#define C_HEAD   0xFF80E0C8U
#define C_LINK   0xFF7FC4FFU
#define C_CODE   0xFFFFD27FU
#define C_DIM    0xFF8090A8U
#define C_HR     0xFF30405AU
#define C_BODYBG 0xFF0D1117U    /* rgba() を合成するときの下地 */

/* 描画命令。kind 0=文字列, 1=水平線, 2=囲み（bg で塗り、color で枠） */
typedef struct { int x, y, w, h, off; short len, link; unsigned char kind, scale, flags; unsigned color, bg; } ht_op;

static ht_op  ops[HT_MAXOPS];
static int    nops;
static char   pool[HT_POOL];
static int    npool;
static int    total_h;

/* ---- リンク（クリックで辿る）。href は pool に置く ---- */
typedef struct { int off; unsigned char kind; } ht_link;   /* kind 0=リンク 1=言語切替 */
static ht_link links[HT_LINKS];
static int nlinks;

/* ---- 走査中の状態 ---- */
static int W, cur_x, cur_y, line_start, line_h, gap, indent, rinset;
static int pend_space, pre_mode, skip_depth, cell_n, any_line;
typedef struct { unsigned flags; unsigned color, bg; unsigned char scale, align, list_kind; int fsize; } ht_style;
static ht_style cur;
typedef struct {
    unsigned char tag, is_block, has_box, list_kind, align;
    ht_style saved;
    int indent_add, rinset_add, list_n;
    int box_op, box_top, box_x, box_w, pb, mb;
    unsigned box_bg, box_border;
    const char *id; int idl; const char *cls; int cll;   /* 選択子の照合用（HTML バッファ内を指す） */
    short link;
} ht_frame;
static ht_frame stk[HT_STACK];
static int sp;
static int cur_link;              /* 今いる <a> のリンク番号（0 なし） */

/* ---- タグ ---- */
enum { T_OTHER=0, T_P, T_H1, T_H2, T_H3, T_H4, T_DIV, T_UL, T_OL, T_LI, T_A, T_B, T_I, T_CODE, T_PRE,
       T_BR, T_HR, T_TABLE, T_TR, T_TD, T_TH, T_BUTTON, T_IMG, T_INPUT, T_SPAN, T_SECTION, T_ARTICLE,
       T_HEADER, T_FOOTER, T_NAV, T_MAIN, T_BLOCKQUOTE, T_SCRIPT, T_STYLE, T_HEAD, T_TITLE, T_SVG,
       T_TEMPLATE, T_NOSCRIPT, T_FORM, T_LABEL, T_DL, T_DT, T_DD, T_SMALL, T_HTML, T_BODY, T_THEAD, T_TBODY,
       T_LINK, T_META, T_SELECT, T_TEXTAREA, T_N };
static const char *tag_names[T_N] = { "", "p","h1","h2","h3","h4","div","ul","ol","li","a","b","i","code","pre",
       "br","hr","table","tr","td","th","button","img","input","span","section","article","header","footer","nav",
       "main","blockquote","script","style","head","title","svg","template","noscript","form","label","dl","dt","dd",
       "small","html","body","thead","tbody","link","meta","select","textarea" };

static int h_len(const char *s){ int n=0; while(s[n]) n++; return n; }
static int h_eqi(const char *a, const char *b, int n)
{ for (int i=0;i<n;i++){ char x=a[i],y=b[i]; if(x>='A'&&x<='Z')x=(char)(x+32); if(y>='A'&&y<='Z')y=(char)(y+32); if(x!=y) return 0; } return 1; }
static int is_ws(char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'; }

static int tag_id(const char *n, int l)
{
    static const struct { const char *s; unsigned char id; } alias[] = {
        {"strong",T_B},{"em",T_I},{"kbd",T_CODE},{"h5",T_H4},{"h6",T_H4},{"aside",T_SECTION},{"figure",T_DIV},
        {"figcaption",T_P},{"details",T_DIV},{"summary",T_P},{"fieldset",T_DIV},{"legend",T_P},{"address",T_P},
        {"tfoot",T_TBODY},{"var",T_CODE},{"samp",T_CODE},{"u",T_SPAN},{"s",T_SPAN},{"sup",T_SMALL},{"sub",T_SMALL},
    };
    for (int i = 1; i < T_N; i++) if (h_len(tag_names[i]) == l && h_eqi(tag_names[i], n, l)) return i;
    for (unsigned i = 0; i < sizeof alias / sizeof alias[0]; i++)
        if (h_len(alias[i].s) == l && h_eqi(alias[i].s, n, l)) return alias[i].id;
    return T_OTHER;
}
static int is_void(int t) { return t==T_BR||t==T_HR||t==T_IMG||t==T_INPUT||t==T_LINK||t==T_META; }
static int is_block_tag(int t)
{ return t==T_P||t==T_H1||t==T_H2||t==T_H3||t==T_H4||t==T_DIV||t==T_UL||t==T_OL||t==T_LI||t==T_PRE||t==T_HR||
         t==T_TABLE||t==T_TR||t==T_SECTION||t==T_ARTICLE||t==T_HEADER||t==T_FOOTER||t==T_NAV||t==T_MAIN||
         t==T_BLOCKQUOTE||t==T_FORM||t==T_DL||t==T_DT||t==T_DD||t==T_BODY||t==T_HTML||t==T_THEAD||t==T_TBODY; }
static int is_skip(int t) { return t==T_SCRIPT||t==T_STYLE||t==T_HEAD||t==T_TITLE||t==T_SVG||t==T_TEMPLATE||t==T_NOSCRIPT||t==T_SELECT||t==T_TEXTAREA; }

/* ---- 属性 ---- */
static int attr_find(const char *a, int al, const char *key, const char **val, int *vl)
{
    int kl = h_len(key);
    for (int i = 0; i + kl <= al; i++) {
        if (!h_eqi(a + i, key, kl)) continue;
        if (i > 0 && !is_ws(a[i-1])) continue;
        int p = i + kl;
        while (p < al && a[p] == ' ') p++;
        if (p >= al || a[p] != '=') { if (p < al && !is_ws(a[p]) && a[p] != '/' && a[p] != '>') continue; *val = a + p; *vl = 0; return 1; }
        p++; while (p < al && a[p] == ' ') p++;
        if (p < al && (a[p] == '"' || a[p] == '\'')) { char q = a[p++]; int st = p; while (p < al && a[p] != q) p++;
            *val = a + st; *vl = p - st; return 1; }
        int st = p; while (p < al && !is_ws(a[p]) && a[p] != '>' && a[p] != '/') p++;
        *val = a + st; *vl = p - st; return 1;
    }
    return 0;
}
static int class_in(const char *v, int vl, const char *cls, int cl)
{
    for (int i = 0; i + cl <= vl; i++)
        if (h_eqi(v + i, cls, cl) && (i == 0 || is_ws(v[i-1])) && (i + cl == vl || is_ws(v[i+cl]))) return 1;
    return 0;
}

/* ====================================================================
 *  CSS
 * ==================================================================== */
typedef struct { const char *tag; int tl; const char *id; int idl; const char *cls[3]; int cll[3]; int ncls; unsigned char child; } ht_comp;
typedef struct { ht_comp comp[4]; int ncomp; int spec; int order; const char *decl; int dl; } ht_rule;
static ht_rule rules[HT_RULES];
static int nrules;
typedef struct { const char *name; int nl; const char *val; int vl; } ht_var;
static ht_var vars[HT_VARS];
static int nvars;
static char css_ext[HT_CSS];      /* 外部 CSS の控え（呼び手が渡す） */
static int  css_ext_len;

/* 計算済みの性質。-1/0 は未指定 */
typedef struct { unsigned color, bg, border; int color_set, bg_set, border_set, transparent;
                 int fsize; int bold; int uline; int align; int display; int mt, mb, ml, pt, pb, pl, pr; int list_none; int upper; } ht_props;

static void props_init(ht_props *p)
{ p->color = p->bg = p->border = 0; p->color_set = p->bg_set = p->border_set = p->transparent = 0;
  p->fsize = 0; p->bold = -1; p->uline = -1; p->align = -1; p->display = -1;
  p->mt = p->mb = p->ml = p->pt = p->pb = p->pl = p->pr = -1; p->list_none = -1; p->upper = -1; }

static const char *skip_ws(const char *s, const char *e) { while (s < e && is_ws(*s)) s++; return s; }

/* 色: #rgb #rrggbb rgb() rgba() 名前 transparent。戻り値 1=色, 2=transparent, 0=不明 */
static int hexv(char c) { if (c>='0'&&c<='9') return c-'0'; if (c>='a'&&c<='f') return c-'a'+10; if (c>='A'&&c<='F') return c-'A'+10; return -1; }
static int parse_color(const char *s, int n, unsigned *out)
{
    const char *e = s + n; s = skip_ws(s, e); while (e > s && is_ws(e[-1])) e--; n = (int)(e - s);
    if (n <= 0) return 0;
    if (s[0] == '#') {
        if (n == 4) { int r=hexv(s[1]),g=hexv(s[2]),b=hexv(s[3]); if (r<0||g<0||b<0) return 0;
                      *out = 0xFF000000U | (unsigned)(r*17) << 16 | (unsigned)(g*17) << 8 | (unsigned)(b*17); return 1; }
        if (n >= 7) { unsigned v = 0; for (int i = 1; i < 7; i++) { int h = hexv(s[i]); if (h < 0) return 0; v = v*16 + (unsigned)h; }
                      *out = 0xFF000000U | v; return 1; }
        return 0;
    }
    if (n >= 4 && h_eqi(s, "rgb", 3)) {
        const char *p = s; while (p < e && *p != '(') p++; if (p >= e) return 0; p++;
        int v[4] = {0,0,0,255}; int k = 0;
        while (p < e && k < 4) {
            p = skip_ws(p, e); int iv = 0, d = 0; int frac = 0, fd = 0;
            while (p < e && *p >= '0' && *p <= '9') { iv = iv*10 + (*p - '0'); p++; d = 1; }
            if (p < e && *p == '.') { p++; while (p < e && *p >= '0' && *p <= '9') { if (fd < 3) { frac = frac*10 + (*p-'0'); fd++; } p++; } }
            if (!d && !fd) break;
            if (k == 3) { int fr = frac; for (int z = fd; z < 3; z++) fr *= 10; v[3] = (iv * 1000 + fr) * 255 / 1000; }
            else v[k] = iv;
            k++;
            while (p < e && (*p == ',' || *p == '/' || is_ws(*p) || *p == '%')) p++;
            if (p < e && *p == ')') break;
        }
        if (k < 3) return 0;
        int a = v[3]; if (a > 255) a = 255; if (a < 0) a = 0;
        /* 下地（本文の背景）と合成する */
        unsigned br = (C_BODYBG >> 16) & 255, bgc = (C_BODYBG >> 8) & 255, bb = C_BODYBG & 255;
        unsigned r = ((unsigned)v[0]*a + br*(255-a)) / 255, g = ((unsigned)v[1]*a + bgc*(255-a)) / 255, b = ((unsigned)v[2]*a + bb*(255-a)) / 255;
        if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
        *out = 0xFF000000U | r << 16 | g << 8 | b; return 1;
    }
    static const struct { const char *nm; unsigned c; } named[] = {
        {"white",0xFFFFFFFF},{"black",0xFF000000},{"red",0xFFFF0000},{"green",0xFF008000},{"blue",0xFF0000FF},
        {"gray",0xFF808080},{"grey",0xFF808080},{"silver",0xFFC0C0C0},{"yellow",0xFFFFFF00},{"orange",0xFFFFA500},
        {"cyan",0xFF00FFFF},{"magenta",0xFFFF00FF},{"lime",0xFF00FF00},{"navy",0xFF000080},{"teal",0xFF008080},
        {"purple",0xFF800080},{"maroon",0xFF800000},{"olive",0xFF808000},{"aqua",0xFF00FFFF},{"fuchsia",0xFFFF00FF},
        {"currentcolor",0},{"inherit",0},
    };
    if (n == 11 && h_eqi(s, "transparent", 11)) return 2;
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
        if (h_len(named[i].nm) == n && h_eqi(s, named[i].nm, n)) { if (!named[i].c) return 0; *out = named[i].c; return 1; }
    return 0;
}

/* 長さ → px。em/% は基準 base（親の文字寸）。不明は -1 */
static int parse_len(const char *s, int n, int base)
{
    const char *e = s + n; s = skip_ws(s, e); while (e > s && is_ws(e[-1])) e--;
    if (s >= e) return -1;
    int neg = 0; if (*s == '-') { neg = 1; s++; }
    int iv = 0, d = 0, frac = 0, fd = 0;
    while (s < e && *s >= '0' && *s <= '9') { iv = iv*10 + (*s-'0'); s++; d = 1; if (iv > 100000) iv = 100000; }
    if (s < e && *s == '.') { s++; while (s < e && *s >= '0' && *s <= '9') { if (fd < 3) { frac = frac*10 + (*s-'0'); fd++; } s++; } }
    if (!d && !fd) return -1;
    for (int z = fd; z < 3; z++) frac *= 10;          /* 1/1000 単位 */
    long v1000 = (long)iv * 1000 + frac;
    long px;
    int ul = (int)(e - s);
    if (ul == 0) px = v1000 / 1000;
    else if (ul == 2 && h_eqi(s, "px", 2)) px = v1000 / 1000;
    else if (ul == 3 && h_eqi(s, "rem", 3)) px = v1000 * 16 / 1000;
    else if (ul == 2 && h_eqi(s, "em", 2)) px = v1000 * base / 1000;
    else if (ul == 1 && *s == '%') px = v1000 * base / 100000;
    else if (ul == 2 && h_eqi(s, "pt", 2)) px = v1000 * 4 / 3000;
    else if (ul == 2 && (h_eqi(s, "vw", 2) || h_eqi(s, "vh", 2))) px = v1000 * 7 / 1000;   /* 700px の窓として */
    else return -1;
    if (neg) px = -px;
    if (px > 4000) px = 4000; if (px < -4000) px = -4000;
    return (int)px;
}

/* var(--x) と clamp(a,b,c) を解いた値を buf に写す（1 段だけ） */
static int resolve_value(const char *s, int n, char *buf, int cap)
{
    int o = 0, i = 0;
    while (i < n && o < cap - 1) {
        if (i + 4 <= n && h_eqi(s + i, "var(", 4)) {
            int p = i + 4, q = p; while (q < n && s[q] != ')' && s[q] != ',') q++;
            const char *nm = s + p; int nl = q - p;
            while (nl > 0 && is_ws(nm[nl-1])) nl--;
            int found = 0;
            for (int k = 0; k < nvars; k++)
                if (vars[k].nl == nl && h_eqi(vars[k].name, nm, nl)) {
                    for (int z = 0; z < vars[k].vl && o < cap - 1; z++) buf[o++] = vars[k].val[z];
                    found = 1; break; }
            if (!found && q < n && s[q] == ',') {           /* 既定値 */
                int r = q + 1; while (r < n && s[r] != ')') { if (o < cap - 1) buf[o++] = s[r]; r++; }
                q = r;
            }
            while (q < n && s[q] != ')') q++;
            i = q + 1; continue;
        }
        if (i + 6 <= n && h_eqi(s + i, "clamp(", 6)) {
            /* clamp(min, pref, max): pref が px/rem なら pref、そうでなければ (min+max)/2 */
            int p = i + 6, depth = 0, part = 0, st = p; const char *a[3]; int al[3] = {0,0,0};
            for (int z = 0; z < 3; z++) a[z] = s + p;
            while (p < n) {
                char c = s[p];
                if (c == '(') depth++;
                else if (c == ')') { if (depth == 0) break; depth--; }
                else if (c == ',' && depth == 0) { if (part < 3) { a[part] = s + st; al[part] = p - st; } part++; st = p + 1; }
                p++;
            }
            if (part < 3) { a[part] = s + st; al[part] = p - st; part++; }
            int mn = parse_len(a[0], al[0], 16), pf = parse_len(a[1], al[1], 16), mx = parse_len(a[2], al[2], 16);
            int v;
            if (al[1] >= 3 && (h_eqi(a[1] + al[1] - 2, "px", 2) || h_eqi(a[1] + al[1] - 3, "rem", 3))) v = pf;
            else if (mn >= 0 && mx >= 0) v = (mn + mx) / 2;
            else v = mn >= 0 ? mn : (mx >= 0 ? mx : 16);
            if (v < 0) v = 0;
            char nb[16]; int k = 0; if (v == 0) nb[k++] = '0'; while (v > 0 && k < 12) { nb[k++] = (char)('0' + v % 10); v /= 10; }
            while (k > 0 && o < cap - 1) buf[o++] = nb[--k];
            if (o < cap - 3) { buf[o++] = 'p'; buf[o++] = 'x'; }
            i = p + 1; continue;
        }
        buf[o++] = s[i++];
    }
    buf[o] = 0;
    return o;
}

/* グラデーションなら最初の色を採る */
static int parse_bg_color(const char *v, int n, unsigned *out)
{
    int r = parse_color(v, n, out);
    if (r) return r;
    for (int i = 0; i + 9 <= n; i++) {
        if (!h_eqi(v + i, "gradient(", 9)) continue;
        int p = i + 9, depth = 0, st = p;
        for (int q = p; q <= n; q++) {
            char c = q < n ? v[q] : ')';
            if (c == '(') { depth++; continue; }
            if (c == ')' && depth > 0) { depth--; continue; }
            if ((c == ',' || c == ')') && depth == 0) {
                const char *a = v + st; const char *ae = v + q; a = skip_ws(a, ae);
                int wl = 0; while (a + wl < ae && !is_ws(a[wl])) wl++;
                if (parse_color(a, wl, out) == 1) return 1;
                if (c == ')') break;
                st = q + 1;
            }
        }
        break;
    }
    return 0;
}

/* 宣言列を props に流し込む。fbase は親の文字寸（em 用） */
static void apply_decls(const char *d, int dl, ht_props *p, int fbase)
{
    int i = 0;
    while (i < dl) {
        while (i < dl && (is_ws(d[i]) || d[i] == ';')) i++;
        int ks = i; while (i < dl && d[i] != ':' && d[i] != ';' && d[i] != '}') i++;
        while (ks < i && is_ws(d[ks])) ks++;
        if (i >= dl || d[i] != ':') { while (i < dl && d[i] != ';') i++; continue; }
        int ke = i; while (ke > ks && is_ws(d[ke-1])) ke--;
        i++; while (i < dl && is_ws(d[i])) i++;         /* ': ' の後の空白（改行も）を飛ばす */
        int vs = i; int q = 0, depth = 0;
        while (i < dl) { char c = d[i]; if (q) { if (c == q) q = 0; } else if (c == '"' || c == '\'') q = (int)c;
                         else if (c == '(') depth++; else if (c == ')') { if (depth) depth--; } else if (c == ';' && depth == 0) break; i++; }
        int ve = i; while (ve > vs && is_ws(d[ve-1])) ve--;
        const char *key = d + ks; int kl = ke - ks;
        static char vb[512]; int vl = resolve_value(d + vs, ve - vs, vb, sizeof vb);
        const char *v = vb;
        for (int z = 0; z + 10 <= vl; z++) if (h_eqi(v + z, "!important", 10)) { vl = z; while (vl > 0 && is_ws(v[vl-1])) vl--; break; }
        #define KEY(s) (kl == (int)h_len(s) && h_eqi(key, s, kl))
        unsigned c;
        if (KEY("color")) { int r = parse_color(v, vl, &c); if (r == 1) { p->color = c; p->color_set = 1; } else if (r == 2) p->transparent = 1; }
        else if (KEY("background") || KEY("background-color")) {
            if (vl >= 4 && h_eqi(v, "none", 4)) { p->bg_set = 0; }
            else { int r = parse_bg_color(v, vl, &c); if (r == 1) { p->bg = c; p->bg_set = 1; } else if (r == 2) p->bg_set = 0; } }
        else if (KEY("border") || KEY("border-bottom") || KEY("border-top") || KEY("outline")) {
            if (vl >= 1 && (h_eqi(v, "none", vl < 4 ? vl : 4) || v[0] == '0')) { p->border_set = 0; }
            else { int z = vl; while (z > 0 && !is_ws(v[z-1])) z--;
                if (parse_color(v + z, vl - z, &c) == 1) { p->border = c; p->border_set = 1; } } }
        else if (KEY("border-color")) { if (parse_color(v, vl, &c) == 1) { p->border = c; p->border_set = 1; } }
        else if (KEY("font-size")) {
            int px = parse_len(v, vl, fbase);
            if (px < 0) { if (vl >= 7 && h_eqi(v, "smaller", 7)) px = fbase * 5 / 6; else if (vl >= 6 && h_eqi(v, "larger", 6)) px = fbase * 6 / 5; }
            if (px > 0) p->fsize = px; }
        else if (KEY("font-weight")) {
            if (vl >= 4 && h_eqi(v, "bold", 4)) p->bold = 1;
            else if (vl >= 6 && h_eqi(v, "normal", 6)) p->bold = 0;
            else { int w = parse_len(v, vl, 0); if (w >= 0) p->bold = (w >= 600); } }
        else if (KEY("text-decoration") || KEY("text-decoration-line")) {
            p->uline = 0; for (int z = 0; z + 9 <= vl; z++) if (h_eqi(v + z, "underline", 9)) { p->uline = 1; break; } }
        else if (KEY("text-align")) {
            if (vl >= 6 && h_eqi(v, "center", 6)) p->align = 1; else if (vl >= 5 && h_eqi(v, "right", 5)) p->align = 2; else p->align = 0; }
        else if (KEY("display")) {
            if (vl >= 4 && h_eqi(v, "none", 4)) p->display = 1;
            else if (vl >= 6 && h_eqi(v, "inline", 6)) p->display = 3;      /* inline / inline-block / inline-flex は行内 */
            else p->display = 2; }
        else if (KEY("visibility")) { if (vl >= 6 && h_eqi(v, "hidden", 6)) p->display = 1; }
        else if (KEY("margin") || KEY("padding")) {
            int isp = (key[0] == 'p' || key[0] == 'P');
            int val[4]; int nv = 0; int z = 0;
            while (z < vl && nv < 4) { while (z < vl && is_ws(v[z])) z++; int st = z; while (z < vl && !is_ws(v[z])) z++;
                if (z > st) { int L = parse_len(v + st, z - st, fbase); val[nv++] = L < 0 ? 0 : L; } }
            if (nv == 0) continue;
            int t = val[0], r = nv > 1 ? val[1] : val[0], b = nv > 2 ? val[2] : val[0], l = nv > 3 ? val[3] : (nv > 1 ? val[1] : val[0]);
            if (isp) { p->pt = t; p->pr = r; p->pb = b; p->pl = l; } else { p->mt = t; p->mb = b; p->ml = l; } }
        else if (KEY("margin-top"))    { int L = parse_len(v, vl, fbase); if (L >= 0) p->mt = L; }
        else if (KEY("margin-bottom")) { int L = parse_len(v, vl, fbase); if (L >= 0) p->mb = L; }
        else if (KEY("margin-left"))   { int L = parse_len(v, vl, fbase); if (L >= 0) p->ml = L; }
        else if (KEY("padding-top"))   { int L = parse_len(v, vl, fbase); if (L >= 0) p->pt = L; }
        else if (KEY("padding-bottom")){ int L = parse_len(v, vl, fbase); if (L >= 0) p->pb = L; }
        else if (KEY("padding-left"))  { int L = parse_len(v, vl, fbase); if (L >= 0) p->pl = L; }
        else if (KEY("padding-right")) { int L = parse_len(v, vl, fbase); if (L >= 0) p->pr = L; }
        else if (KEY("list-style") || KEY("list-style-type")) { p->list_none = (vl >= 4 && h_eqi(v, "none", 4)); }
        else if (KEY("text-transform")) { p->upper = (vl >= 9 && h_eqi(v, "uppercase", 9)); }
        #undef KEY
    }
}

/* 選択子の複合部分を読む。戻り値 0=この規則は捨てる */
static int parse_compound(const char *s, int n, ht_comp *c)
{
    c->tag = 0; c->tl = 0; c->id = 0; c->idl = 0; c->ncls = 0; c->child = 0;
    int i = 0;
    if (i < n && s[i] == '*') i++;
    else { int st = i; while (i < n && ((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||(s[i]>='0'&&s[i]<='9')||s[i]=='-')) i++;
           if (i > st) { c->tag = s + st; c->tl = i - st; } }
    while (i < n) {
        if (s[i] == '.') { i++; int st = i; while (i < n && s[i] != '.' && s[i] != '#' && s[i] != ':' && s[i] != '[') i++;
            if (c->ncls < 3 && i > st) { c->cls[c->ncls] = s + st; c->cll[c->ncls] = i - st; c->ncls++; } }
        else if (s[i] == '#') { i++; int st = i; while (i < n && s[i] != '.' && s[i] != '#' && s[i] != ':' && s[i] != '[') i++;
            if (i > st) { c->id = s + st; c->idl = i - st; } }
        else if (s[i] == ':') { if (n - i == 5 && h_eqi(s + i, ":root", 5) && i == 0) { c->tag = "html"; c->tl = 4; return 1; }
                                return 0; }   /* 擬似クラス・擬似要素は捨てる（:hover など） */
        else if (s[i] == '[') return 0;
        else i++;
    }
    return 1;
}
static void add_rule(const char *sel, int sl, const char *decl, int dl)
{
    if (nrules >= HT_RULES) return;
    ht_rule *r = &rules[nrules];
    r->ncomp = 0; r->decl = decl; r->dl = dl; r->order = nrules;
    int i = 0, spec = 0;
    while (i < sl) {
        while (i < sl && is_ws(sel[i])) i++;
        int child = 0;
        if (i < sl && sel[i] == '>') { child = 1; i++; while (i < sl && is_ws(sel[i])) i++; }
        if (i < sl && (sel[i] == '+' || sel[i] == '~')) return;   /* 兄弟結合子は扱わない */
        int st = i; while (i < sl && !is_ws(sel[i]) && sel[i] != '>') i++;
        if (i == st) break;
        if (r->ncomp >= 4) return;
        if (!parse_compound(sel + st, i - st, &r->comp[r->ncomp])) return;
        r->comp[r->ncomp].child = (unsigned char)child;
        ht_comp *c = &r->comp[r->ncomp];
        spec += (c->id ? 100 : 0) + c->ncls * 10 + (c->tl ? 1 : 0);
        r->ncomp++;
    }
    if (r->ncomp == 0) return;
    r->spec = spec;
    nrules++;
}
static void parse_css(const char *css, int n)
{
    int i = 0;
    while (i < n) {
        while (i < n && is_ws(css[i])) i++;
        if (i + 1 < n && css[i] == '/' && css[i+1] == '*') { i += 2; while (i + 1 < n && !(css[i] == '*' && css[i+1] == '/')) i++; i += 2; continue; }
        if (i < n && css[i] == '@') {                   /* @media 等: 中身は捨てる（画面幅の条件は解けない） */
            int depth = 0; int p = i;
            while (p < n) { if (css[p] == '{') depth++; else if (css[p] == '}') { depth--; if (depth <= 0) { p++; break; } } else if (css[p] == ';' && depth == 0) { p++; break; } p++; }
            i = p; continue;
        }
        int ss = i; while (i < n && css[i] != '{' && css[i] != '}') i++;
        if (i >= n) break;
        if (css[i] == '}') { i++; continue; }
        int se = i; i++;
        int ds = i; int depth = 1;
        while (i < n) { if (css[i] == '{') depth++; else if (css[i] == '}') { depth--; if (depth == 0) break; } i++; }
        int de = i; if (i < n) i++;
        { const char *s = css + ss; int sl = se - ss; while (sl > 0 && is_ws(s[sl-1])) sl--;
          if (sl == 5 && h_eqi(s, ":root", 5)) {
              int p = ds;
              while (p < de) {
                  while (p < de && (is_ws(css[p]) || css[p] == ';')) p++;
                  if (p + 2 < de && css[p] == '-' && css[p+1] == '-') {
                      int ns = p; while (p < de && css[p] != ':') p++;
                      int ne = p; while (ne > ns && is_ws(css[ne-1])) ne--;
                      p++; int vs = p; while (p < de && css[p] != ';') p++;
                      int ve = p; while (ve > vs && is_ws(css[vs])) vs++; while (ve > vs && is_ws(css[ve-1])) ve--;
                      if (nvars < HT_VARS) { vars[nvars].name = css + ns; vars[nvars].nl = ne - ns; vars[nvars].val = css + vs; vars[nvars].vl = ve - vs; nvars++; }
                  } else { while (p < de && css[p] != ';') p++; }
              }
          } }
        { int p = ss;
          while (p < se) {
              int st = p; while (p < se && css[p] != ',') p++;
              int en = p; while (st < en && is_ws(css[st])) st++; while (en > st && is_ws(css[en-1])) en--;
              if (en > st) add_rule(css + st, en - st, css + ds, de - ds);
              p++;
          } }
    }
}

/* 複合選択子が枠 f に合うか */
static int comp_match(const ht_comp *c, const ht_frame *f)
{
    if (c->tl) { const char *tn = tag_names[f->tag];
        if (!tn[0] || h_len(tn) != c->tl || !h_eqi(tn, c->tag, c->tl)) {
            if (!(f->tag == T_B && c->tl == 6 && h_eqi(c->tag, "strong", 6)) && !(f->tag == T_I && c->tl == 2 && h_eqi(c->tag, "em", 2))) return 0; } }
    if (c->id) { if (!f->id || f->idl != c->idl || !h_eqi(f->id, c->id, c->idl)) return 0; }
    for (int k = 0; k < c->ncls; k++) if (!f->cls || !class_in(f->cls, f->cll, c->cls[k], c->cll[k])) return 0;
    return 1;
}
static int rule_match(const ht_rule *r, int top)
{
    int ci = r->ncomp - 1, fi = top;
    if (!comp_match(&r->comp[ci], &stk[fi])) return 0;
    ci--; fi--;
    while (ci >= 0) {
        if (r->comp[ci + 1].child) { if (fi < 0 || !comp_match(&r->comp[ci], &stk[fi])) return 0; ci--; fi--; continue; }
        while (fi >= 0 && !comp_match(&r->comp[ci], &stk[fi])) fi--;
        if (fi < 0) return 0;
        ci--; fi--;
    }
    return 1;
}
/* スタック先頭の要素の性質を計算する（規則を詳細度→出現順に適用、最後に style 属性） */
static void compute_props(int top, const char *a, int al, ht_props *p, int fbase)
{
    props_init(p);
    for (int pass = 0; pass < 3; pass++) {
        int lo = pass == 0 ? 0 : (pass == 1 ? 10 : 100), hi = pass == 0 ? 9 : (pass == 1 ? 99 : 100000);
        for (int i = 0; i < nrules; i++) {
            if (rules[i].spec < lo || rules[i].spec > hi) continue;
            if (rule_match(&rules[i], top)) apply_decls(rules[i].decl, rules[i].dl, p, fbase);
        }
    }
    const char *v; int vl;
    if (attr_find(a, al, "style", &v, &vl) && vl > 0) apply_decls(v, vl, p, fbase);
    if (attr_find(a, al, "hidden", &v, &vl)) p->display = 1;
}

/* ====================================================================
 *  行の組み立て
 * ==================================================================== */
static void flush_line(void)
{
    if (nops == line_start) return;
    if (any_line) cur_y += gap;
    gap = 0; any_line = 1;
    int align = cur.align;
    if (align) {
        int right = 0; for (int i = line_start; i < nops; i++) if (ops[i].x + ops[i].w > right) right = ops[i].x + ops[i].w;
        int avail = W - rinset - right;
        int shift = align == 1 ? avail / 2 : avail;
        if (shift > 0) for (int i = line_start; i < nops; i++) ops[i].x += shift;
    }
    for (int i = line_start; i < nops; i++) { ops[i].y = cur_y; if (ops[i].kind == 0 && ops[i].h < line_h) ops[i].y += (line_h - ops[i].h); }
    cur_y += line_h + LINE_GAP;
    line_start = nops; cur_x = indent; line_h = 0; pend_space = 0;
}
static void want_gap(int g) { if (g > GAP_MAX) g = GAP_MAX; if (g > gap) gap = g; }
static void newline(void) { flush_line(); cur_x = indent; }

/* ---- フォント ---- */
extern const int jp_count;
extern const unsigned short jp_uni[];
extern const unsigned char jp_bits[][32];
extern const unsigned char asc16_bits[95][16];
static int jp_index(unsigned long cp)
{
    if (cp < 0x80 || cp > 0xFFFF) return -1;
    int lo = 0, hi = jp_count - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (jp_uni[m] == cp) return m; if (jp_uni[m] < cp) lo = m + 1; else hi = m - 1; }
    return -1;
}
static int utf8_get(const char *s, int n, unsigned long *cp)
{
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int ext; unsigned long v;
    if ((c & 0xE0) == 0xC0) { v = c & 0x1F; ext = 1; } else if ((c & 0xF0) == 0xE0) { v = c & 0x0F; ext = 2; }
    else if ((c & 0xF8) == 0xF0) { v = c & 0x07; ext = 3; } else { *cp = '?'; return 1; }
    if (ext >= n) { *cp = '?'; return 1; }
    for (int k = 1; k <= ext; k++) { if (((unsigned char)s[k] & 0xC0) != 0x80) { *cp = '?'; return 1; } v = (v << 6) | ((unsigned char)s[k] & 0x3F); }
    *cp = v; return ext + 1;
}
static int char_w(unsigned long cp, int scale, int bold) { return ((cp < 0x80) ? AW : JW) * scale + (bold ? 1 : 0); }
static int run_w(const char *s, int n, int scale, int bold)
{ int w = 0, i = 0; while (i < n) { unsigned long cp; i += utf8_get(s + i, n - i, &cp); w += char_w(cp, scale, bold); } return w; }

static void put_run(const char *s, int n)   /* UTF-8 の一語（途中で折り返さない単位） */
{
    if (n <= 0 || nops >= HT_MAXOPS || npool + n + 1 >= HT_POOL) return;
    int bold = (cur.flags & ST_BOLD) != 0;
    int sw = AW * cur.scale + (bold ? 1 : 0);
    int w = run_w(s, n, cur.scale, bold);
    if (cur_x > indent && cur_x + (pend_space ? sw : 0) + w > W - rinset && !(cur.flags & ST_NOWRAP)) newline();
    else if (pend_space && cur_x > indent) cur_x += sw;
    pend_space = 0;
    int guard = 0;
    while (n > 0 && guard++ < 512) {
        int take = 0, tw = 0;
        while (take < n) { unsigned long cp; int l = utf8_get(s + take, n - take, &cp); int cw = char_w(cp, cur.scale, bold);
                           if (take > 0 && cur_x + tw + cw > W - rinset) break; take += l; tw += cw; }
        if (nops >= HT_MAXOPS || npool + take + 1 >= HT_POOL) return;
        ht_op *o = &ops[nops++];
        o->x = cur_x; o->y = 0; o->w = tw; o->h = FH * cur.scale; o->len = (short)take; o->kind = 0;
        o->scale = cur.scale; o->flags = (unsigned char)cur.flags; o->color = cur.color; o->bg = cur.bg; o->link = (short)cur_link; o->off = npool;
        for (int i = 0; i < take; i++) pool[npool++] = ((cur.flags & ST_UPPER) && s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
        pool[npool++] = 0;
        cur_x += tw;
        if (FH * cur.scale > line_h) line_h = FH * cur.scale;
        s += take; n -= take;
        if (n > 0) newline();
    }
}
static void put_ascii(const char *s, unsigned flags, unsigned color)
{
    ht_style sv = cur; cur.flags = flags; cur.color = color; cur.bg = 0;
    put_run(s, h_len(s)); cur = sv;
}
static void put_hr(void)
{
    newline(); want_gap(6);
    if (nops < HT_MAXOPS) { ht_op *o = &ops[nops++]; o->x = indent; o->y = 0; o->w = W - rinset - indent; o->h = 1; o->len = 0;
                            o->kind = 1; o->scale = 1; o->flags = 0; o->color = C_HR; o->bg = 0; o->link = 0; o->off = 0; line_h = 1; }
    newline(); want_gap(6);
}

/* ---- 文字：実体参照と UTF-8。フォントに無い記号は ASCII に写す ---- */
static int cp_to_ascii(unsigned long cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    switch (cp) {
    case 0xA0: out[0]=' '; return 1;
    case 0xA9: out[0]='('; out[1]='c'; out[2]=')'; return 3;
    case 0xAB: out[0]='<'; out[1]='<'; return 2;
    case 0xBB: out[0]='>'; out[1]='>'; return 2;
    case 0xB7: case 0x2022: out[0]='*'; return 1;
    case 0x2013: case 0x2014: case 0x2015: case 0x2212: out[0]='-'; return 1;
    case 0x2018: case 0x2019: case 0x2032: out[0]='\''; return 1;
    case 0x201C: case 0x201D: out[0]='"'; return 1;
    case 0x2026: out[0]='.'; out[1]='.'; out[2]='.'; return 3;
    case 0x2192: out[0]='-'; out[1]='>'; return 2;
    case 0x2190: out[0]='<'; out[1]='-'; return 2;
    case 0x2194: case 0x21C6: case 0x21C4: out[0]='<'; out[1]='-'; out[2]='>'; return 3;
    case 0x21D2: out[0]='='; out[1]='>'; return 2;
    case 0x2713: case 0x2714: out[0]='v'; return 1;
    case 0x2717: case 0x2718: out[0]='x'; return 1;
    default: break;
    }
    if (cp >= 0x2460 && cp <= 0x2473) { unsigned n = (unsigned)(cp - 0x2460 + 1);
        int k = 0; out[k++]='('; if (n >= 10) out[k++]=(char)('0'+n/10); out[k++]=(char)('0'+n%10); out[k++]=')'; return k; }
    out[0] = '.'; return 1;
}
static int entity(const char *s, int n, unsigned long *cp)
{
    static const struct { const char *nm; unsigned short cp; } et[] = {
        {"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''},{"nbsp",0xA0},{"copy",0xA9},
        {"mdash",0x2014},{"ndash",0x2013},{"hellip",0x2026},{"rarr",0x2192},{"larr",0x2190},{"harr",0x2194},
        {"times",0xD7},{"middot",0xB7},{"bull",0x2022},{"laquo",0xAB},{"raquo",0xBB},{"lsquo",0x2018},
        {"rsquo",0x2019},{"ldquo",0x201C},{"rdquo",0x201D},{"tau",0x3C4},{"lambda",0x3BB},{"alpha",0x3B1},
        {"beta",0x3B2},{"pi",0x3C0},{"le",0x2264},{"ge",0x2265},{"ne",0x2260},{"rArr",0x21D2},{"check",0x2713},
        {"deg",0xB0},{"plusmn",0xB1},{"divide",0xF7},{"infin",0x221E},
    };
    if (n > 0 && s[0] == '#') {
        unsigned long v = 0; int i = 1, hex = 0;
        if (i < n && (s[i] == 'x' || s[i] == 'X')) { hex = 1; i++; }
        int d = 0;
        for (; i < n && i < 10; i++) {
            char c = s[i];
            if (c >= '0' && c <= '9') v = v * (hex ? 16 : 10) + (unsigned long)(c - '0');
            else if (hex && c >= 'a' && c <= 'f') v = v * 16 + (unsigned long)(c - 'a' + 10);
            else if (hex && c >= 'A' && c <= 'F') v = v * 16 + (unsigned long)(c - 'A' + 10);
            else break;
            d = 1;
        }
        if (!d) return 0;
        *cp = v;
        return (i < n && s[i] == ';') ? i + 1 : i;
    }
    for (unsigned k = 0; k < sizeof et / sizeof et[0]; k++) {
        int l = h_len(et[k].nm);
        if (l < n && s[0] == et[k].nm[0] && h_eqi(s, et[k].nm, l) && s[l] == ';') { *cp = et[k].cp; return l + 1; }
    }
    return 0;
}
/* 行頭に来てはいけない文字（閉じ括弧・句読点・小書き・長音） */
static int kinsoku_tail(unsigned long cp)
{
    switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF09: case 0x300D: case 0x300F: case 0x3011: case 0x3015:
    case 0x3009: case 0x300B: case 0x2019: case 0x201D: case 0x309D: case 0x309E: case 0x3005: case 0x30FC: case 0x30FB:
    case 0xFF1A: case 0xFF1B: case 0xFF01: case 0xFF1F: case 0xFF5D: case 0xFF3D:
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063: case 0x3083: case 0x3085: case 0x3087: case 0x308E:
    case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9: case 0x30C3: case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE:
        return 1;
    default: return 0; }
}
static int kinsoku_head(unsigned long cp)   /* 行末に来てはいけない（開き括弧） */
{ return cp==0xFF08||cp==0x300C||cp==0x300E||cp==0x3010||cp==0x3014||cp==0x3008||cp==0x300A||cp==0x2018||cp==0x201C||cp==0xFF5B||cp==0xFF3B; }

static void put_text(const char *s, int n, int keep_ws)
{
    static char wbuf[512]; int wl = 0;
    int i = 0, glue = 0;            /* glue: 直前が開き括弧（次の字を同じ語に含める） */
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        char tmp[8]; int tn = 0; unsigned long cp = 0; int wide = 0;
        if (c == '&') {
            int used = entity(s + i + 1, n - i - 1, &cp);
            if (used) i += 1 + used; else { cp = '&'; i++; }
        } else if (c >= 0x80) {
            i += utf8_get(s + i, n - i, &cp);
            if (cp == 0x200B || cp == 0xFEFF) continue;
        } else { cp = c; i++; }
        if (cp == 0x3000) cp = ' ';                       /* 全角空白は空白として扱う */
        if (cp >= 0x80 && jp_index(cp) >= 0) {
            wide = 1;
            if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); tn = 2; }
            else { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); tn = 3; }
        }
        if (!wide) tn = cp_to_ascii(cp, tmp);

        if (wide) {
            /* 全角 1 字 = 1 語（行頭禁則の字は前の語に付ける、開き括弧は次の字と結ぶ） */
            /* 直前の字は wbuf に残しておく（次に来る句読点を同じ語に付けるため）。
               行頭禁則の字か、直前が開き括弧なら足す。そうでなければ前の語を出してから足す。 */
            if (wl + tn >= (int)sizeof wbuf - 1) { put_run(wbuf, wl); wl = 0; }
            if (!(kinsoku_tail(cp) || glue) && wl) { put_run(wbuf, wl); wl = 0; }
            for (int k = 0; k < tn; k++) wbuf[wl++] = tmp[k];
            glue = kinsoku_head(cp);
            continue;
        }
        for (int k = 0; k < tn; k++) {
            char ch = tmp[k];
            int ws = (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f');
            if (keep_ws && ch == '\n') { if (wl) { put_run(wbuf, wl); wl = 0; } newline(); glue = 0; continue; }
            if (ws) {
                if (wl) { put_run(wbuf, wl); wl = 0; }
                if (keep_ws) { cur_x += AW * cur.scale; if (FH * cur.scale > line_h) line_h = FH * cur.scale; }
                else pend_space = 1;
                glue = 0; continue;
            }
            if ((unsigned char)ch < 0x20) ch = '?';
            if (wl < (int)sizeof wbuf - 4) wbuf[wl++] = ch;
            else { put_run(wbuf, wl); wl = 0; wbuf[wl++] = ch; }
            glue = 0;
        }
    }
    if (wl) put_run(wbuf, wl);
}

/* ====================================================================
 *  要素の開閉
 * ==================================================================== */
static int size_to_scale(int px) { return px <= 20 ? 1 : (px <= 34 ? 2 : 3); }

static void open_tag(int t, const char *a, int al)
{
    if (sp >= HT_STACK) return;
    ht_frame *f = &stk[sp];
    f->tag = (unsigned char)t; f->is_block = 0; f->has_box = 0; f->list_kind = 0; f->list_n = 0; f->align = cur.align;
    f->saved = cur; f->indent_add = 0; f->rinset_add = 0; f->pb = 0; f->mb = 0; f->link = (short)cur_link;
    { const char *v; int vl;
      f->id = 0; f->idl = 0; f->cls = 0; f->cll = 0;
      if (attr_find(a, al, "id", &v, &vl)) { f->id = v; f->idl = vl; }
      if (attr_find(a, al, "class", &v, &vl)) { f->cls = v; f->cll = vl; } }
    sp++;
    if (is_skip(t)) { skip_depth++; return; }
    if (skip_depth) return;

    ht_props p;
    int fbase = cur.fsize > 0 ? cur.fsize : 16;
    compute_props(sp - 1, a, al, &p, fbase);
    if (p.display == 1) { skip_depth++; f->has_box = 2; return; }

    int block = is_block_tag(t);
    if (p.display == 2 && !is_void(t)) block = 1;
    if (p.display == 3) block = 0;
    int mt = -1, mb = -1;
    unsigned flags = cur.flags; unsigned color = cur.color; int fsize = cur.fsize; int list_kind = 0;
    switch (t) {
    case T_H1: mt = 18; mb = 10; fsize = 40; flags |= ST_BOLD; color = C_HEAD; break;
    case T_H2: mt = 16; mb = 8;  fsize = 28; flags |= ST_BOLD; color = C_HEAD; break;
    case T_H3: mt = 10; mb = 4;  fsize = 18; flags |= ST_BOLD; color = C_HEAD; break;
    case T_H4: mt = 8;  mb = 4;  flags |= ST_BOLD; break;
    case T_P:  mt = 6;  mb = 6;  break;
    case T_SECTION: case T_ARTICLE: case T_HEADER: case T_FOOTER: case T_MAIN: mt = 8; mb = 8; break;
    case T_NAV: mt = 4; mb = 4; break;
    case T_UL: case T_OL: mt = 4; mb = 4; f->indent_add = 20; break;
    case T_DD: f->indent_add = 20; break;
    case T_DT: mt = 4; flags |= ST_BOLD; break;
    case T_BLOCKQUOTE: mt = 4; mb = 4; f->indent_add = 16; color = C_DIM; break;
    case T_PRE: mt = 6; mb = 6; flags |= ST_CODE; color = C_CODE; break;
    case T_TABLE: mt = 6; mb = 8; break;
    case T_A: flags |= ST_ULINE; color = C_LINK; break;
    case T_B: flags |= ST_BOLD; break;
    case T_I: case T_SMALL: color = C_DIM; break;
    case T_CODE: flags |= ST_CODE; color = C_CODE; break;
    case T_BUTTON: flags |= ST_BTN; break;
    case T_TH: flags |= ST_BOLD; break;
    default: break;
    }
    if (t == T_UL || t == T_OL) list_kind = t == T_OL ? 2 : 1;
    if (p.transparent) { p.color_set = 0; p.bg_set = 0; }   /* background-clip:text の見出し：色も背景も触らない */
    if (p.color_set) color = p.color;
    if (p.fsize > 0) fsize = p.fsize;
    if (p.bold == 1) flags |= ST_BOLD; else if (p.bold == 0) flags &= ~ST_BOLD;
    if (p.uline == 1) flags |= ST_ULINE; else if (p.uline == 0) flags &= ~ST_ULINE;
    if (p.upper == 1) flags |= ST_UPPER; else if (p.upper == 0) flags &= ~ST_UPPER;
    if (p.mt >= 0) mt = p.mt; if (p.mb >= 0) mb = p.mb;
    if (p.align >= 0) f->align = (unsigned char)p.align;
    if (p.list_none == 1 && list_kind) list_kind = 3;
    if (p.ml > 0 && block) f->indent_add += p.ml > 48 ? 48 : p.ml;

    unsigned bg = cur.bg;
    if (block) {
        f->is_block = 1;
        newline();
        if (mt >= 0) want_gap(mt);
        cur.align = f->align;
        if (p.bg_set || p.border_set) {
            if (any_line) cur_y += gap; gap = 0;
            f->has_box = 1; f->box_op = nops; f->box_top = cur_y; f->box_x = indent; f->box_w = W - rinset - indent;
            f->box_bg = p.bg_set ? p.bg : 0; f->box_border = p.border_set ? p.border : 0;
            int pt = p.pt >= 0 ? (p.pt > GAP_MAX ? GAP_MAX : p.pt) : 6, pl = p.pl >= 0 ? (p.pl > 32 ? 32 : p.pl) : 8, pr = p.pr >= 0 ? (p.pr > 32 ? 32 : p.pr) : 8;
            f->pb = p.pb >= 0 ? (p.pb > GAP_MAX ? GAP_MAX : p.pb) : 6;
            cur_y += pt; f->indent_add += pl; f->rinset_add += pr; any_line = 0;
            bg = 0;
        } else {
            if (p.pt > 0) want_gap(p.pt);
            f->pb = p.pb > 0 ? p.pb : 0;
            if (p.pl > 0) f->indent_add += p.pl > 32 ? 32 : p.pl;
        }
        f->mb = mb >= 0 ? mb : 0;
        indent += f->indent_add; rinset += f->rinset_add;
        if (cur_x < indent) cur_x = indent;
    } else if (p.bg_set) bg = p.bg;

    cur.flags = flags; cur.color = color; cur.bg = block ? 0 : bg; cur.fsize = fsize; cur.scale = (unsigned char)size_to_scale(fsize);
    f->list_kind = (unsigned char)list_kind;

    switch (t) {
    case T_LI: {
        int kind = 1, n = 0;
        for (int i = sp - 2; i >= 0; i--) if (stk[i].list_kind) { kind = stk[i].list_kind; n = ++stk[i].list_n; break; }
        if (kind != 3) {
            char b[8]; int bl = 0;
            if (kind == 2) { if (n >= 10) b[bl++] = (char)('0' + n / 10); b[bl++] = (char)('0' + n % 10); b[bl++] = '.'; }
            else { b[bl++] = '*'; }
            b[bl] = 0;
            int bx = indent - 16; if (bx < 0) bx = 0;
            cur_x = bx; put_ascii(b, 0, C_HEAD); cur_x = indent; pend_space = 0;
        }
        break; }
    case T_TR: cell_n = 0; break;
    case T_TD: case T_TH: if (cell_n++ > 0) { pend_space = 1; put_ascii("|", 0, C_DIM); pend_space = 1; } break;
    case T_A: {
        const char *v; int vl;
        if (attr_find(a, al, "href", &v, &vl) && vl > 0 && nlinks < HT_LINKS && npool + vl + 1 < HT_POOL) {
            links[nlinks].off = npool; links[nlinks].kind = 0;
            for (int i = 0; i < vl; i++) pool[npool++] = v[i];
            pool[npool++] = 0;
            cur_link = ++nlinks;
        }
        if (f->cls && class_in(f->cls, f->cll, "btn", 3)) { cur.flags |= ST_BTN; pend_space = 1; put_ascii("[", 0, C_DIM); pend_space = 1; }
        break; }
    case T_BUTTON:
        if (f->id && f->idl == 11 && h_eqi(f->id, "lang-toggle", 11) && nlinks < HT_LINKS) {
            links[nlinks].off = 0; links[nlinks].kind = 1; cur_link = ++nlinks;
        }
        pend_space = 1; put_ascii("[", 0, C_DIM); pend_space = 1; break;
    case T_SPAN:
        if (f->cls && class_in(f->cls, f->cll, "chip", 4)) { cur.flags |= ST_BTN; pend_space = 1; put_ascii("[", 0, C_DIM); pend_space = 1; }
        break;
    case T_BR: newline(); break;
    case T_HR: put_hr(); break;
    case T_IMG: { const char *v; int vl;
        put_ascii("[", 0, C_DIM);
        if (attr_find(a, al, "alt", &v, &vl) && vl > 0) put_text(v, vl, 0); else put_ascii("img", 0, C_DIM);
        put_ascii("]", 0, C_DIM); break; }
    case T_INPUT: { const char *v; int vl;
        if (attr_find(a, al, "type", &v, &vl) && vl == 6 && h_eqi(v, "hidden", 6)) break;
        put_ascii("[", 0, C_DIM);
        if (attr_find(a, al, "value", &v, &vl) && vl > 0) put_text(v, vl, 0);
        else if (attr_find(a, al, "placeholder", &v, &vl) && vl > 0) { unsigned sc = cur.color; cur.color = C_DIM; put_text(v, vl, 0); cur.color = sc; }
        else put_ascii("______", 0, C_DIM);
        put_ascii("]", 0, C_DIM); break; }
    default: break;
    }
    if (t == T_PRE) pre_mode++;
}

static void close_frame(void)
{
    sp--;
    ht_frame *f = &stk[sp];
    int tt = f->tag;
    if (tt == T_PRE && pre_mode) pre_mode--;
    if (f->has_box == 2) { if (skip_depth) skip_depth--; cur = f->saved; return; }   /* display:none */
    if (is_skip(tt)) { if (skip_depth) skip_depth--; cur = f->saved; return; }
    if (skip_depth) { cur = f->saved; return; }

    if (tt == T_BUTTON || ((tt == T_A || tt == T_SPAN) && (cur.flags & ST_BTN))) {
        pend_space = (cur_x + 2 * AW * cur.scale <= W - rinset);
        put_ascii("]", 0, C_DIM); pend_space = 1;
    }
    if (tt == T_A) cur_link = f->link;
    if (f->is_block) {
        newline();
        if (f->has_box == 1) {
            cur_y += f->pb;
            int h = cur_y - f->box_top;
            if (nops < HT_MAXOPS && h > 0 && f->box_op <= nops) {
                for (int i = nops; i > f->box_op; i--) ops[i] = ops[i-1];
                nops++;
                ht_op *o = &ops[f->box_op];
                o->x = f->box_x; o->y = f->box_top; o->w = f->box_w; o->h = h; o->len = 0; o->kind = 2;
                o->scale = 1; o->flags = 0; o->color = f->box_border; o->bg = f->box_bg; o->link = 0; o->off = 0;
                line_start = nops;
                /* 外側の囲みの差し込み位置も 1 つ後ろへ */
                for (int i = 0; i < sp; i++) if (stk[i].has_box == 1 && stk[i].box_op > f->box_op) stk[i].box_op++;
            }
            any_line = 1;
            gap = 0;
        } else if (f->pb > 0) want_gap(f->pb);
        want_gap(f->mb);
        indent -= f->indent_add; rinset -= f->rinset_add;
        if (indent < 0) indent = 0; if (rinset < 0) rinset = 0;
        cur_x = indent;
    }
    cur = f->saved;
}
static void close_tag(int t)
{
    int i = sp - 1;
    while (i >= 0 && stk[i].tag != t) i--;
    if (i < 0) return;
    while (sp > i) close_frame();
}

/* ====================================================================
 *  入口
 * ==================================================================== */
void html_set_css(const char *css, int n)
{
    if (n > HT_CSS - 1) n = HT_CSS - 1;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) css_ext[i] = css[i];
    css_ext[n] = 0; css_ext_len = n;
}

void html_layout(const char *h, int n, int width_px)
{
    nops = 0; npool = 0; total_h = 0; nlinks = 0; nrules = 0; nvars = 0;
    W = width_px; if (W < 96) W = 96;
    cur_x = 0; cur_y = 0; line_start = 0; line_h = 0; gap = 0; indent = 0; rinset = 0;
    pend_space = 0; pre_mode = 0; skip_depth = 0; cell_n = 0; any_line = 0;
    sp = 0; cur_link = 0;
    cur.flags = 0; cur.color = C_FG; cur.bg = 0; cur.scale = 1; cur.align = 0; cur.list_kind = 0; cur.fsize = 16;
    if (!h || n <= 0) return;
    if (css_ext_len > 0) parse_css(css_ext, css_ext_len);
    for (int i = 0; i + 7 < n; i++) {                 /* <style> は本文より後ろにあっても効かせる */
        if (h[i] != '<' || !h_eqi(h + i + 1, "style", 5) || !(is_ws(h[i+6]) || h[i+6] == '>')) continue;
        int p = i + 6; while (p < n && h[p] != '>') p++; p++;
        int q = p; while (q + 7 <= n && !h_eqi(h + q, "</style", 7)) q++;
        if (q > p && q <= n) parse_css(h + p, q - p);
        i = q;
    }

    int i = 0;
    while (i < n) {
        if (h[i] != '<') {
            int st = i; while (i < n && h[i] != '<') i++;
            if (!skip_depth) put_text(h + st, i - st, pre_mode);
            continue;
        }
        if (i + 3 < n && h[i+1] == '!' && h[i+2] == '-' && h[i+3] == '-') {
            i += 4; while (i + 2 < n && !(h[i] == '-' && h[i+1] == '-' && h[i+2] == '>')) i++;
            i += 3; if (i > n) i = n; continue;
        }
        if (i + 1 < n && (h[i+1] == '!' || h[i+1] == '?')) { while (i < n && h[i] != '>') i++; i++; continue; }
        int closing = 0; int p = i + 1;
        if (p < n && h[p] == '/') { closing = 1; p++; }
        int ns = p;
        while (p < n && ((h[p]>='a'&&h[p]<='z')||(h[p]>='A'&&h[p]<='Z')||(h[p]>='0'&&h[p]<='9')||h[p]=='-')) p++;
        int nl = p - ns;
        if (nl == 0) { if (!skip_depth) put_text("<", 1, 0); i++; continue; }
        int as = p;
        int q = 0;
        while (p < n) { char c = h[p]; if (q) { if (c == q) q = 0; } else if (c == '"' || c == '\'') q = (int)c; else if (c == '>') break; p++; }
        int ae = p; if (ae > n) ae = n;
        int self = (ae > as && h[ae-1] == '/');
        int t = tag_id(h + ns, nl);
        if (closing) close_tag(t);
        else {
            if (t == T_SCRIPT || t == T_STYLE) {
                const char *end = (t == T_SCRIPT) ? "</script" : "</style"; int el = h_len(end);
                int k = ae + 1; while (k + el <= n && !h_eqi(h + k, end, el)) k++;
                while (k < n && h[k] != '>') k++;
                i = k + 1; if (i > n) i = n; continue;
            }
            open_tag(t, h + as, ae - as);
            if (is_void(t) || self) close_tag(t);
        }
        i = ae + 1;
    }
    while (sp > 0) close_frame();
    newline();
    total_h = cur_y;
}

int html_height(void) { return total_h; }
int html_ops(void)    { return nops; }

/* 点 (x,y)（整形座標）にあるリンク。戻り値 0=なし 1=リンク（*href に URL） 2=言語切替 */
int html_link_at(int x, int y, const char **href)
{
    for (int i = 0; i < nops; i++) {
        ht_op *o = &ops[i];
        if (o->kind != 0 || !o->link) continue;
        if (x >= o->x - 2 && x < o->x + o->w + 2 && y >= o->y - 2 && y < o->y + o->h + 2) {
            ht_link *l = &links[o->link - 1];
            if (l->kind == 1) return 2;
            *href = pool + l->off; return 1;
        }
    }
    return 0;
}

/* <link rel="stylesheet" href="..."> の最初の href を返す（0=無し） */
int html_find_stylesheet(const char *h, int n, char *out, int cap)
{
    for (int i = 0; i + 5 < n; i++) {
        if (h[i] != '<' || !h_eqi(h + i + 1, "link", 4)) continue;
        int p = i + 5; while (p < n && h[p] != '>') p++;
        const char *v; int vl;
        if (attr_find(h + i + 5, p - (i + 5), "rel", &v, &vl) && vl >= 10 && h_eqi(v, "stylesheet", 10) &&
            attr_find(h + i + 5, p - (i + 5), "href", &v, &vl) && vl > 0 && vl < cap) {
            for (int k = 0; k < vl; k++) out[k] = v[k]; out[vl] = 0; return vl; }
        i = p;
    }
    return 0;
}

/* ====================================================================
 *  描画
 * ==================================================================== */
extern void draw_bitmap_glyph(int px, int py, const unsigned char *rows, int w, int h, unsigned int fg, unsigned int bg, int scale, int transparent);
extern void fill_rect(int x, int y, int w, int h, unsigned int c);

static void draw_run(int px, int py, const char *s, int n, unsigned fg, unsigned bg, int scale, int bold)
{
    int i = 0;
    while (i < n) {
        unsigned long cp; i += utf8_get(s + i, n - i, &cp);
        const unsigned char *rows; int w;
        if (cp < 0x80) { rows = asc16_bits[(cp < 0x20 || cp > 0x7E) ? '?' - 0x20 : cp - 0x20]; w = AW; }
        else { int k = jp_index(cp); rows = k >= 0 ? jp_bits[k] : asc16_bits['?' - 0x20]; w = k >= 0 ? JW : AW; }
        draw_bitmap_glyph(px, py, rows, w, FH, fg, bg, scale, 0);
        if (bold) draw_bitmap_glyph(px + 1, py, rows, w, FH, fg, 0, scale, 1);
        px += w * scale + (bold ? 1 : 0);
    }
}

void html_draw(int x0, int y0, int w, int h, int scroll_y, unsigned int bg)
{
    for (int i = 0; i < nops; i++) {
        ht_op *o = &ops[i];
        int y = o->y - scroll_y;
        if (o->kind == 2) {
            int top = y < 0 ? 0 : y, bot = y + o->h > h ? h : y + o->h;
            if (bot <= top || o->x >= w) continue;
            int bw = o->x + o->w > w ? w - o->x : o->w;
            if (o->bg) fill_rect(x0 + o->x, y0 + top, bw, bot - top, o->bg);
            if (o->color) {
                if (y >= 0) fill_rect(x0 + o->x, y0 + y, bw, 1, o->color);
                if (y + o->h - 1 < h) fill_rect(x0 + o->x, y0 + y + o->h - 1, bw, 1, o->color);
                fill_rect(x0 + o->x, y0 + top, 1, bot - top, o->color);
                fill_rect(x0 + o->x + bw - 1, y0 + top, 1, bot - top, o->color);
            }
            continue;
        }
        if (y < 0 || y + o->h > h) continue;
        if (o->x + o->w > w) continue;
        int px = x0 + o->x, py = y0 + y;
        if (o->kind == 1) { fill_rect(px, py, o->w, 1, o->color); continue; }
        unsigned fg = o->color ? o->color : C_FG;
        unsigned cellbg = bg;
        if (o->bg) { cellbg = o->bg; fill_rect(px, py, o->w, o->h, cellbg); }
        else { for (int k = i - 1; k >= 0; k--) { ht_op *b = &ops[k]; if (b->kind == 2 && o->y >= b->y && o->y < b->y + b->h && o->x >= b->x && o->x < b->x + b->w && b->bg) { cellbg = b->bg; break; } } }
        draw_run(px, py, pool + o->off, o->len, fg, cellbg, o->scale, (o->flags & ST_BOLD) != 0);
        if (o->flags & ST_ULINE) fill_rect(px, py + o->h - 1, o->w, 1, fg);
    }
}

/* ---- 文字に落とす（検算用） ---- */
int html_text(char *dst, int cap)
{
    int o = 0, last_y = -1, last_end = 0, last_h = FH;
    for (int i = 0; i < nops && o < cap - 2; i++) {
        ht_op *p = &ops[i];
        if (p->kind == 2) continue;
        if (p->y != last_y) {
            if (last_y >= 0) { dst[o++] = '\n'; if (p->y - last_y > last_h + LINE_GAP + 4 && o < cap - 2) dst[o++] = '\n'; }
            last_y = p->y; last_end = 0; last_h = (p->kind == 1) ? 1 : p->h;
            for (int k = 0; k < p->x / AW && o < cap - 2; k++) dst[o++] = ' ';
        } else if (p->x > last_end + 2 && o < cap - 2) dst[o++] = ' ';
        if (p->kind == 1) { for (int k = 0; k < 40 && o < cap - 2; k++) dst[o++] = '-'; last_end = p->x + p->w; continue; }
        const char *s = pool + p->off;
        for (int k = 0; k < p->len && o < cap - 2; k++) dst[o++] = s[k];
        last_end = p->x + p->w;
    }
    if (o < cap - 1) dst[o++] = '\n';
    dst[o] = 0;
    return o;
}

/* ====================================================================
 *  英語版 HTML の組み立て（構造を保って data-i18n の中身だけ差し替える）
 * ==================================================================== */
static int dict_lookup(const char *dict, int dl, const char *key, char *dst, int cap)
{
    int en = -1;
    for (int i = 0; i + 6 < dl; i++)
        if (dict[i]=='e' && dict[i+1]=='n' && dict[i+2]==':' && dict[i+3]==' ' && dict[i+4]=='{') { en = i; break; }
    if (en < 0) return -1;
    int kl = h_len(key), at = 0;
    for (int i = en; i + kl + 3 < dl; i++) {
        if (dict[i] != '\'') continue;
        int m = 1;
        for (int k = 0; k < kl; k++) if (dict[i+1+k] != key[k]) { m = 0; break; }
        if (!m || dict[i+1+kl] != '\'') continue;
        int p = i + kl + 2;
        while (p < dl && (dict[p]==' '||dict[p]==':'||dict[p]=='\n'||dict[p]=='\r')) p++;
        int guard = 0;
        while (p < dl && dict[p]=='\'' && guard++ < 40) {
            p++;
            while (p < dl && dict[p] != '\'' && at < cap - 1) {
                if (dict[p]=='\\' && p+1 < dl) p++;
                dst[at++] = dict[p++];
            }
            while (p < dl && dict[p] != '\'') p++;
            if (p < dl) p++;
            while (p < dl && (dict[p]==' '||dict[p]=='+'||dict[p]=='\n'||dict[p]=='\r')) p++;
        }
        dst[at] = 0;
        return at;
    }
    return -1;
}

int html_i18n_apply(const char *h, int n, const char *dict, int dl, char *out, int cap)
{
    static char val[4096];
    int o = 0, i = 0;
    const char *tag = "data-i18n=\""; int tl = h_len(tag);
    while (i < n && o < cap - 1) {
        if (h[i] != '<') { out[o++] = h[i++]; continue; }
        int p = i + 1, q = 0;
        while (p < n) { char c = h[p]; if (q) { if (c == q) q = 0; } else if (c == '"' || c == '\'') q = (int)c; else if (c == '>') break; p++; }
        if (p >= n) { while (i < n && o < cap - 1) out[o++] = h[i++]; break; }
        int te = p;
        int kpos = -1;
        for (int k = i; k + tl < te; k++) if (h_eqi(h + k, tag, tl)) { kpos = k + tl; break; }
        for (int k = i; k <= te && o < cap - 1; k++) out[o++] = h[k];
        i = te + 1;
        if (kpos < 0 || (te > 0 && h[te-1] == '/')) continue;
        char key[64]; int kn = 0;
        while (kpos < te && h[kpos] != '"' && kn < 63) key[kn++] = h[kpos++];
        key[kn] = 0;
        int depth = 0, j = i, inner_end = -1;
        while (j < n) {
            if (h[j] != '<') { j++; continue; }
            if (j + 1 < n && h[j+1] == '/') {
                if (depth == 0) { inner_end = j; break; }
                depth--;
                while (j < n && h[j] != '>') j++; j++; continue;
            }
            if (j + 3 < n && h[j+1] == '!' && h[j+2] == '-' && h[j+3] == '-') { j += 4; while (j + 2 < n && !(h[j]=='-'&&h[j+1]=='-'&&h[j+2]=='>')) j++; j += 3; continue; }
            int ns = j + 1; while (ns < n && ((h[ns]>='a'&&h[ns]<='z')||(h[ns]>='A'&&h[ns]<='Z')||(h[ns]>='0'&&h[ns]<='9'))) ns++;
            int t = tag_id(h + j + 1, ns - (j + 1));
            int e = ns, q2 = 0;
            while (e < n) { char c = h[e]; if (q2) { if (c == q2) q2 = 0; } else if (c == '"' || c == '\'') q2 = (int)c; else if (c == '>') break; e++; }
            int selfc = (e > ns && h[e-1] == '/');
            if (!is_void(t) && !selfc) depth++;
            j = e + 1;
        }
        if (inner_end < 0) continue;
        int vl = dict_lookup(dict, dl, key, val, sizeof val);
        if (vl < 0) continue;
        for (int k = 0; k < vl && o < cap - 1; k++) out[o++] = val[k];
        i = inner_end;
    }
    out[o] = 0;
    return o;
}
