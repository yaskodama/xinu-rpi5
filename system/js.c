/* system/js.c —— 機内 JavaScript 処理系（部分集合）
 *
 * 目的は「JS 一般を動かす」ことではなく、**板のブラウザが実際のページを
 * 組み立てられるようにする**ことである。そこで作る前に、対象
 * （airilab.app の /js/i18n.js, 670 行）が何を使っているかを数えた:
 *
 *   使う   : const/let, アロー関数, function, 三項, IIFE, オブジェクト/配列リテラル,
 *            計算メンバ参照 obj[k], typeof, 文字列連結, if, return,
 *            localStorage, document.querySelectorAll(...).forEach,
 *            getAttribute, innerHTML, addEventListener, documentElement
 *   使わない: class, async/await, 分割代入, JSON.*, Object.*, Array.*,
 *            map/filter/split/join, オプショナル連鎖 ?. , ??
 *   （class/await/... は全部**文字列の中**にあった。数えずに作ると
 *     要らない機能に時間を使い、要る機能を落とす。）
 *
 * 数は整数（long）のみ。対象は小数を使わない。浮動小数が要る場面が出たら
 * そのとき足す ―― 先に入れると、この板では FP 退避の議論を抱え込む。
 */

/* ===== 領域 ============================================================= */
#define JS_SRCCAP   65536
/* ★ 領域について。最初は 64KB / 6000 節点で足りると見込んだが、実物
 * （i18n.js 37KB、文字列リテラルだけで 31KB、識別子が延べ 3,353 個）で足りなかった。
 * しかも js_intern は満杯のとき **黙って 0（＝空文字列）を返して**いたので、
 * 途中から全トークンが空になり「構文: 記号が合いません」として現れた ――
 * 本当の原因（領域不足）はどこにも出ていなかった。
 * 対策は 3 つ: (1) 同じ文字列は使い回す(重複を潰す)、(2) 余裕を持たせる、
 * (3) **あふれたら黙らず失敗させる**。(3) が一番大事である。 */
#define JS_STRCAP   163840
#define JS_NODECAP  16000
#define JS_PROPCAP  4096
#define JS_OBJCAP   1024
#define JS_ENVCAP   512
#define JS_VARCAP   2048
#define JS_HASH     2048

typedef struct { unsigned char t; long n; int s; int o; } jsv;
enum { JT_UNDEF = 0, JT_NULL, JT_BOOL, JT_NUM, JT_STR, JT_OBJ, JT_FUN, JT_NAT };

static char  js_str[JS_STRCAP];  static int js_str_n;
static char  js_err[128];        static int js_has_err;
static int   js_err_pos = -1;    /* 失敗したときの原文の位置 */
static const char *js_src; static int js_srclen, js_pos;
static struct { int k; long num; int str; int p0, p1; } js_tk;

static void js_fail(const char *m);   /* 前方宣言（intern からあふれを報せる） */

/* 同じ文字列は使い回す。識別子は何度も現れるので、潰さないと領域が尽きる。
 * 走査を避けるため小さなハッシュ表を置く（衝突は連鎖で追う）。 */
static int js_hash_head[JS_HASH];
static int js_hash_next[JS_STRCAP / 8];
static int js_hash_n;

static int js_intern(const char *s, int len)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    int b = (int)(h & (JS_HASH - 1));
    for (int i = js_hash_head[b]; i >= 0; i = js_hash_next[i]) {
        /* i は「登録番号」。対応する文字列の先頭は js_hash_off[i]。 */
        extern int js_hash_off_get(int);
        int off = js_hash_off_get(i);
        const char *x = js_str + off;
        int k = 0; while (k < len && x[k] == s[k]) k++;
        if (k == len && x[len] == 0) return off;
    }
    if (js_str_n + len + 1 >= JS_STRCAP) { js_fail("文字列領域があふれました"); return 0; }
    int at = js_str_n;
    for (int i = 0; i < len; i++) js_str[at + i] = s[i];
    js_str[at + len] = 0;
    js_str_n += len + 1;
    if (js_hash_n < (int)(sizeof js_hash_next / sizeof js_hash_next[0])) {
        extern void js_hash_off_set(int, int);
        js_hash_off_set(js_hash_n, at);
        js_hash_next[js_hash_n] = js_hash_head[b];
        js_hash_head[b] = js_hash_n;
        js_hash_n++;
    }
    return at;
}
static int js_hash_off[JS_STRCAP / 8];
int  js_hash_off_get(int i) { return js_hash_off[i]; }
void js_hash_off_set(int i, int v) { js_hash_off[i] = v; }
static int js_cstr(const char *s) { int n = 0; while (s[n]) n++; return js_intern(s, n); }
static int js_seq(int a, const char *b)
{ const char *x = js_str + a; int i = 0; while (x[i] && x[i] == b[i]) i++; return x[i] == 0 && b[i] == 0; }

static void js_fail(const char *m)
{ if (js_has_err) return; js_has_err = 1; js_err_pos = js_tk.p0;
  int i = 0; while (m[i] && i < 127) { js_err[i] = m[i]; i++; } js_err[i] = 0; }

/* ===== 字句解析 ========================================================= */
enum { T_EOF=0, T_NUM, T_STR, T_TMPL, T_NAME, T_PUNCT, T_KW };
typedef struct { int k; long num; int str; int p0, p1; } jtok;



static int js_isid0(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$'||(unsigned char)c>=0x80; }
static int js_isid (char c){ return js_isid0(c)||(c>='0'&&c<='9'); }
static int js_isdig(char c){ return c>='0'&&c<='9'; }

static void js_skip(void)
{
    for (;;) {
        while (js_pos < js_srclen) {
            char c = js_src[js_pos];
            if (c==' '||c=='\t'||c=='\r'||c=='\n') js_pos++;
            else break;
        }
        if (js_pos + 1 < js_srclen && js_src[js_pos]=='/' && js_src[js_pos+1]=='/') {
            while (js_pos < js_srclen && js_src[js_pos] != '\n') js_pos++;
            continue;
        }
        if (js_pos + 1 < js_srclen && js_src[js_pos]=='/' && js_src[js_pos+1]=='*') {
            js_pos += 2;
            while (js_pos + 1 < js_srclen && !(js_src[js_pos]=='*'&&js_src[js_pos+1]=='/')) js_pos++;
            js_pos += 2;
            continue;
        }
        break;
    }
}

static const char *js_kw[] = { "var","let","const","function","return","if","else","for","while",
                               "true","false","null","undefined","typeof","new","this","break","continue", 0 };

static void js_next(void)
{
    js_skip();
    js_tk.p0 = js_pos;
    if (js_pos >= js_srclen) { js_tk.k = T_EOF; js_tk.p1 = js_pos; return; }
    char c = js_src[js_pos];

    if (js_isdig(c)) {                                   /* 数（整数のみ） */
        long v = 0;
        while (js_pos < js_srclen && js_isdig(js_src[js_pos])) v = v*10 + (js_src[js_pos++]-'0');
        js_tk.k = T_NUM; js_tk.num = v; js_tk.p1 = js_pos; return;
    }
    if (c=='\''||c=='"') {                               /* 文字列 */
        char q = c; js_pos++;
        static char buf[8192]; int n = 0;
        while (js_pos < js_srclen && js_src[js_pos] != q) {
            char d = js_src[js_pos++];
            if (d=='\\' && js_pos < js_srclen) {
                char e = js_src[js_pos++];
                if (e=='n') d='\n'; else if (e=='t') d='\t'; else if (e=='r') d='\r'; else d=e;
            }
            if (n < 8190) buf[n++] = d;
        }
        js_pos++;
        js_tk.k = T_STR; js_tk.str = js_intern(buf, n); js_tk.p1 = js_pos; return;
    }
    if (c=='`') {                                        /* テンプレート（${} は評価しない） */
        js_pos++;
        static char buf[4096]; int n = 0;
        while (js_pos < js_srclen && js_src[js_pos] != '`') {
            if (js_src[js_pos]=='$' && js_pos+1 < js_srclen && js_src[js_pos+1]=='{') {
                int depth = 1; js_pos += 2;
                while (js_pos < js_srclen && depth) { if (js_src[js_pos]=='{') depth++;
                                                      else if (js_src[js_pos]=='}') depth--; js_pos++; }
                continue;
            }
            if (n < 4094) buf[n++] = js_src[js_pos];
            js_pos++;
        }
        js_pos++;
        js_tk.k = T_TMPL; js_tk.str = js_intern(buf, n); js_tk.p1 = js_pos; return;
    }
    if (js_isid0(c)) {                                   /* 名前・予約語 */
        int st = js_pos;
        while (js_pos < js_srclen && js_isid(js_src[js_pos])) js_pos++;
        js_tk.str = js_intern(js_src+st, js_pos-st);
        js_tk.k = T_NAME;
        for (int i = 0; js_kw[i]; i++) if (js_seq(js_tk.str, js_kw[i])) { js_tk.k = T_KW; break; }
        js_tk.p1 = js_pos; return;
    }
    /* 記号。長いものから照合する */
    { static const char *ops[] = { "===","!==","=>","==","!=","<=",">=","&&","||","++","--","+=","-=", 0 };
      for (int i = 0; ops[i]; i++) {
          int L = 0; while (ops[i][L]) L++;
          if (js_pos + L <= js_srclen) {
              int m = 1; for (int k = 0; k < L; k++) if (js_src[js_pos+k] != ops[i][k]) { m = 0; break; }
              if (m) { js_tk.k = T_PUNCT; js_tk.str = js_intern(ops[i], L); js_pos += L; js_tk.p1 = js_pos; return; }
          } } }
    { char one[2]; one[0] = c; one[1] = 0;
      js_tk.k = T_PUNCT; js_tk.str = js_intern(one, 1); js_pos++; js_tk.p1 = js_pos; return; }
}

static int js_is(const char *s){ return (js_tk.k==T_PUNCT||js_tk.k==T_KW) && js_seq(js_tk.str, s); }
static int js_eat(const char *s){ if (js_is(s)) { js_next(); return 1; } return 0; }
static void js_expect(const char *s){ if (!js_eat(s)) js_fail("構文: 記号が合いません"); }

/* ===== 構文木 =========================================================== */
enum { N_NUM=1, N_STR, N_NAME, N_BOOL, N_NULL, N_UNDEF, N_OBJ, N_ARR,
       N_BIN, N_UN, N_ASSIGN, N_COND, N_CALL, N_MEMBER, N_INDEX,
       N_FUN, N_ARROW, N_RETURN, N_IF, N_BLOCK, N_VAR, N_EXPR, N_WHILE, N_FOR, N_SEQ };

typedef struct { unsigned char k; int a, b, c, d; long num; int str; } jnode;
static jnode js_nd[JS_NODECAP]; static int js_nd_n;

static int js_mk(int k)
{ if (js_nd_n >= JS_NODECAP) { js_fail("構文木があふれました"); return 0; }
  int i = js_nd_n++; js_nd[i].k=(unsigned char)k; js_nd[i].a=js_nd[i].b=js_nd[i].c=js_nd[i].d=-1;
  js_nd[i].num=0; js_nd[i].str=0; return i; }

/* 子の並びは「連結リスト」を d で作る（可変長を配列にしないで済ませる）。 */
static int js_parse_expr(void);
static int js_parse_stmt(void);

static int js_parse_args(void)      /* '(' の次から。式の並びを返す */
{
    int head = -1, tail = -1;
    if (!js_is(")")) for (;;) {
        int e = js_parse_expr();
        int cell = js_mk(N_SEQ); js_nd[cell].a = e;
        if (tail < 0) head = cell; else js_nd[tail].d = cell;
        tail = cell;
        if (!js_eat(",")) break;
    }
    js_expect(")");
    return head;
}

static int js_parse_params(void)    /* '(' の次から。名前の並び */
{
    int head = -1, tail = -1;
    if (!js_is(")")) for (;;) {
        if (js_tk.k != T_NAME) { js_fail("構文: 仮引数名"); break; }
        int cell = js_mk(N_SEQ); js_nd[cell].str = js_tk.str; js_next();
        if (tail < 0) head = cell; else js_nd[tail].d = cell;
        tail = cell;
        if (!js_eat(",")) break;
    }
    js_expect(")");
    return head;
}

static int js_parse_primary(void)
{
    if (js_has_err) return 0;
    if (js_tk.k == T_NUM)  { int n = js_mk(N_NUM); js_nd[n].num = js_tk.num; js_next(); return n; }
    if (js_tk.k == T_STR || js_tk.k == T_TMPL)
                           { int n = js_mk(N_STR); js_nd[n].str = js_tk.str; js_next(); return n; }
    if (js_is("true")||js_is("false")) { int n = js_mk(N_BOOL); js_nd[n].num = js_is("true"); js_next(); return n; }
    if (js_eat("null"))      return js_mk(N_NULL);
    if (js_eat("undefined")) return js_mk(N_UNDEF);
    if (js_is("function")) {
        js_next();
        int n = js_mk(N_FUN);
        if (js_tk.k == T_NAME) { js_nd[n].str = js_tk.str; js_next(); }
        js_expect("("); js_nd[n].a = js_parse_params();
        js_nd[n].b = js_parse_stmt();
        return n;
    }
    if (js_eat("(")) {
        /* アロー関数か括弧式か。'(' の対応を数えて '=>' が続くかで決める。 */
        int save_pos = js_tk.p0, depth = 1, p = js_tk.p0, is_arrow = 0;
        while (p < js_srclen && depth) { char c = js_src[p];
            if (c=='(') depth++; else if (c==')') depth--; p++; }
        { int q = p; while (q < js_srclen && (js_src[q]==' '||js_src[q]=='\n'||js_src[q]=='\r'||js_src[q]=='\t')) q++;
          if (q+1 < js_srclen && js_src[q]=='=' && js_src[q+1]=='>') is_arrow = 1; }
        (void)save_pos;
        if (is_arrow) {
            int n = js_mk(N_ARROW);
            js_nd[n].a = js_parse_params();
            js_expect("=>");
            if (js_is("{")) js_nd[n].b = js_parse_stmt();
            else { int r = js_mk(N_RETURN); js_nd[r].a = js_parse_expr(); js_nd[n].b = r; }
            return n;
        }
        int e = js_parse_expr(); js_expect(")"); return e;
    }
    if (js_eat("{")) {                                   /* オブジェクトリテラル */
        int n = js_mk(N_OBJ); int head = -1, tail = -1;
        if (!js_is("}")) for (;;) {
            int key;
            if (js_tk.k == T_STR || js_tk.k == T_NAME || js_tk.k == T_KW) { key = js_tk.str; js_next(); }
            else if (js_tk.k == T_NUM) { static char b[24]; long v = js_tk.num; int m=0;
                                         if(!v) b[m++]='0'; while(v){b[m++]=(char)('0'+v%10);v/=10;}
                                         for(int i=0;i<m/2;i++){char t=b[i];b[i]=b[m-1-i];b[m-1-i]=t;}
                                         key = js_intern(b,m); js_next(); }
            else { js_fail("構文: オブジェクトの鍵"); break; }
            js_expect(":");
            int val = js_parse_expr();
            int cell = js_mk(N_SEQ); js_nd[cell].str = key; js_nd[cell].a = val;
            if (tail < 0) head = cell; else js_nd[tail].d = cell;
            tail = cell;
            if (!js_eat(",")) break;
            if (js_is("}")) break;                        /* 末尾コンマ */
        }
        js_expect("}");
        js_nd[n].a = head; return n;
    }
    if (js_eat("[")) {                                   /* 配列リテラル */
        int n = js_mk(N_ARR); int head = -1, tail = -1;
        if (!js_is("]")) for (;;) {
            int e = js_parse_expr();
            int cell = js_mk(N_SEQ); js_nd[cell].a = e;
            if (tail < 0) head = cell; else js_nd[tail].d = cell;
            tail = cell;
            if (!js_eat(",")) break;
            if (js_is("]")) break;
        }
        js_expect("]");
        js_nd[n].a = head; return n;
    }
    if (js_tk.k == T_NAME || js_is("this")) {
        int nm = js_tk.str; int p1 = js_tk.p1; js_next();
        if (js_is("=>")) {                                /* 引数1個のアロー */
            js_next();
            int n = js_mk(N_ARROW);
            int cell = js_mk(N_SEQ); js_nd[cell].str = nm; js_nd[n].a = cell;
            if (js_is("{")) js_nd[n].b = js_parse_stmt();
            else { int r = js_mk(N_RETURN); js_nd[r].a = js_parse_expr(); js_nd[n].b = r; }
            return n;
        }
        (void)p1;
        int n = js_mk(N_NAME); js_nd[n].str = nm; return n;
    }
    js_fail("構文: 式が読めません");
    return js_mk(N_UNDEF);
}

static int js_parse_postfix(void)
{
    int e = js_parse_primary();
    for (;;) {
        if (js_has_err) return e;
        if (js_eat(".")) {
            if (js_tk.k != T_NAME && js_tk.k != T_KW) { js_fail("構文: プロパティ名"); return e; }
            int n = js_mk(N_MEMBER); js_nd[n].a = e; js_nd[n].str = js_tk.str; js_next(); e = n;
        } else if (js_eat("[")) {
            int n = js_mk(N_INDEX); js_nd[n].a = e; js_nd[n].b = js_parse_expr(); js_expect("]"); e = n;
        } else if (js_eat("(")) {
            int n = js_mk(N_CALL); js_nd[n].a = e; js_nd[n].b = js_parse_args(); e = n;
        } else break;
    }
    return e;
}

static int js_parse_unary(void)
{
    if (js_is("!")||js_is("-")||js_is("typeof")) {
        int op = js_tk.str; js_next();
        int n = js_mk(N_UN); js_nd[n].str = op; js_nd[n].a = js_parse_unary(); return n;
    }
    return js_parse_postfix();
}

/* 二項の優先順位。数が大きいほど強く結ぶ。 */
static int js_prec(int s)
{
    if (js_seq(s,"*")||js_seq(s,"/")||js_seq(s,"%")) return 7;
    if (js_seq(s,"+")||js_seq(s,"-")) return 6;
    if (js_seq(s,"<")||js_seq(s,">")||js_seq(s,"<=")||js_seq(s,">=")) return 5;
    if (js_seq(s,"==")||js_seq(s,"!=")||js_seq(s,"===")||js_seq(s,"!==")) return 4;
    if (js_seq(s,"&&")) return 3;
    if (js_seq(s,"||")) return 2;
    return 0;
}

static int js_parse_bin(int minp)
{
    int lhs = js_parse_unary();
    for (;;) {
        if (js_tk.k != T_PUNCT || js_has_err) break;
        int p = js_prec(js_tk.str);
        if (p == 0 || p < minp) break;
        int op = js_tk.str; js_next();
        int rhs = js_parse_bin(p + 1);
        int n = js_mk(N_BIN); js_nd[n].str = op; js_nd[n].a = lhs; js_nd[n].b = rhs; lhs = n;
    }
    return lhs;
}

static int js_parse_expr(void)
{
    int e = js_parse_bin(1);
    if (js_eat("?")) {
        int n = js_mk(N_COND); js_nd[n].a = e; js_nd[n].b = js_parse_expr();
        js_expect(":"); js_nd[n].c = js_parse_expr(); return n;
    }
    if (js_is("=")||js_is("+=")) {
        int op = js_tk.str; js_next();
        int n = js_mk(N_ASSIGN); js_nd[n].str = op; js_nd[n].a = e; js_nd[n].b = js_parse_expr(); return n;
    }
    return e;
}

static int js_parse_stmt(void)
{
    if (js_has_err) return js_mk(N_UNDEF);
    if (js_eat("{")) {
        int n = js_mk(N_BLOCK); int head = -1, tail = -1;
        while (!js_is("}") && js_tk.k != T_EOF && !js_has_err) {
            int s = js_parse_stmt();
            int cell = js_mk(N_SEQ); js_nd[cell].a = s;
            if (tail < 0) head = cell; else js_nd[tail].d = cell;
            tail = cell;
        }
        js_expect("}");
        js_nd[n].a = head; return n;
    }
    if (js_is("var")||js_is("let")||js_is("const")) {
        js_next();
        int n = js_mk(N_VAR); int head = -1, tail = -1;
        for (;;) {
            if (js_tk.k != T_NAME) { js_fail("構文: 変数名"); break; }
            int nm = js_tk.str; js_next();
            int init = -1;
            if (js_eat("=")) init = js_parse_expr();
            int cell = js_mk(N_SEQ); js_nd[cell].str = nm; js_nd[cell].a = init;
            if (tail < 0) head = cell; else js_nd[tail].d = cell;
            tail = cell;
            if (!js_eat(",")) break;
        }
        js_eat(";");
        js_nd[n].a = head; return n;
    }
    if (js_eat("return")) {
        int n = js_mk(N_RETURN);
        if (!js_is(";") && !js_is("}")) js_nd[n].a = js_parse_expr();
        js_eat(";"); return n;
    }
    if (js_eat("if")) {
        int n = js_mk(N_IF); js_expect("("); js_nd[n].a = js_parse_expr(); js_expect(")");
        js_nd[n].b = js_parse_stmt();
        if (js_eat("else")) js_nd[n].c = js_parse_stmt();
        return n;
    }
    if (js_eat("while")) {
        int n = js_mk(N_WHILE); js_expect("("); js_nd[n].a = js_parse_expr(); js_expect(")");
        js_nd[n].b = js_parse_stmt(); return n;
    }
    if (js_eat("for")) {                                  /* for(init;cond;step) だけ */
        int n = js_mk(N_FOR); js_expect("(");
        if (!js_is(";")) js_nd[n].a = js_parse_stmt(); else js_eat(";");
        if (!js_is(";")) js_nd[n].b = js_parse_expr(); js_expect(";");
        if (!js_is(")")) js_nd[n].c = js_parse_expr(); js_expect(")");
        js_nd[n].d = js_parse_stmt(); return n;
    }
    if (js_is("function")) return js_parse_primary();
    if (js_eat(";")) return js_mk(N_UNDEF);
    { int n = js_mk(N_EXPR); js_nd[n].a = js_parse_expr(); js_eat(";"); return n; }
}

/* ===== 値・オブジェクト・環境 =========================================== */
static struct { int used; int cls; } js_obj[JS_OBJCAP];   /* cls: 0=普通 1=配列 2=DOM 節点 */
static int js_obj_n;
static struct { int obj, key; jsv val; int used; } js_prop[JS_PROPCAP]; static int js_prop_n;
static struct { int parent; int first; } js_env[JS_ENVCAP]; static int js_env_n;
static struct { int env, name, next; jsv val; } js_var[JS_VARCAP]; static int js_var_n;

static jsv JU(void){ jsv v; v.t=JT_UNDEF; v.n=0; v.s=0; v.o=0; return v; }
static jsv JB(int b){ jsv v=JU(); v.t=JT_BOOL; v.n=b?1:0; return v; }
static jsv JN(long n){ jsv v=JU(); v.t=JT_NUM; v.n=n; return v; }
static jsv JS_(int s){ jsv v=JU(); v.t=JT_STR; v.s=s; return v; }

static int js_new_obj(int cls)
{ if (js_obj_n >= JS_OBJCAP) { js_fail("オブジェクトがあふれました"); return 0; }
  int i = js_obj_n++; js_obj[i].used = 1; js_obj[i].cls = cls; return i; }

static jsv *js_find_prop(int obj, int key)
{ for (int i = 0; i < js_prop_n; i++)
      if (js_prop[i].used && js_prop[i].obj == obj && js_seq(js_prop[i].key, js_str + key))
          return &js_prop[i].val;
  return 0; }

static void js_set_prop(int obj, int key, jsv v)
{ jsv *p = js_find_prop(obj, key); if (p) { *p = v; return; }
  if (js_prop_n >= JS_PROPCAP) { js_fail("プロパティがあふれました"); return; }
  js_prop[js_prop_n].obj = obj; js_prop[js_prop_n].key = key;
  js_prop[js_prop_n].val = v; js_prop[js_prop_n].used = 1; js_prop_n++; }

static jsv js_get_prop(int obj, int key)
{ jsv *p = js_find_prop(obj, key); return p ? *p : JU(); }

static int js_new_env(int parent)
{ if (js_env_n >= JS_ENVCAP) { js_fail("環境があふれました"); return 0; }
  int i = js_env_n++; js_env[i].parent = parent; js_env[i].first = -1; return i; }

static void js_declare(int env, int name, jsv v)
{ if (js_var_n >= JS_VARCAP) { js_fail("変数があふれました"); return; }
  int i = js_var_n++; js_var[i].env = env; js_var[i].name = name; js_var[i].val = v;
  js_var[i].next = js_env[env].first; js_env[env].first = i; }

static jsv *js_lookup(int env, int name)
{ for (int e = env; e >= 0; e = js_env[e].parent)
      for (int i = js_env[e].first; i >= 0; i = js_var[i].next)
          if (js_seq(js_var[i].name, js_str + name)) return &js_var[i].val;
  return 0; }

/* ===== 変換 ============================================================= */
static int js_truthy(jsv v)
{ switch (v.t) { case JT_UNDEF: case JT_NULL: return 0;
                 case JT_BOOL: case JT_NUM: return v.n != 0;
                 case JT_STR: return js_str[v.s] != 0;
                 default: return 1; } }

static int js_numstr(long v)
{ char b[24]; int n = 0, neg = v < 0; if (neg) v = -v;
  if (!v) b[n++]='0'; while (v) { b[n++] = (char)('0'+v%10); v/=10; }
  if (neg) b[n++]='-';
  char t[24]; for (int i=0;i<n;i++) t[i]=b[n-1-i];
  return js_intern(t, n); }

static int js_tostr(jsv v)
{ switch (v.t) {
    case JT_STR:  return v.s;
    case JT_NUM:  return js_numstr(v.n);
    case JT_BOOL: return js_cstr(v.n ? "true" : "false");
    case JT_NULL: return js_cstr("null");
    case JT_FUN: case JT_NAT: return js_cstr("[function]");
    case JT_OBJ:  return js_cstr(js_obj[v.o].cls == 1 ? "[array]" : "[object]");
    default:      return js_cstr("undefined"); } }

static int js_str_concat(int a, int b)
{ const char *x = js_str + a, *y = js_str + b;
  int la = 0; while (x[la]) la++; int lb = 0; while (y[lb]) lb++;
  if (js_str_n + la + lb + 1 >= JS_STRCAP) return a;
  int at = js_str_n;
  for (int i=0;i<la;i++) js_str[at+i]=x[i];
  for (int i=0;i<lb;i++) js_str[at+la+i]=y[i];
  js_str[at+la+lb] = 0; js_str_n += la+lb+1; return at; }

static int js_eqv(jsv a, jsv b)
{ if (a.t != b.t) {
      if ((a.t==JT_NULL&&b.t==JT_UNDEF)||(a.t==JT_UNDEF&&b.t==JT_NULL)) return 1;
      return 0; }
  switch (a.t) { case JT_STR: return js_seq(a.s, js_str + b.s);
                 case JT_NUM: case JT_BOOL: return a.n == b.n;
                 case JT_OBJ: case JT_FUN: return a.o == b.o;
                 default: return 1; } }

/* ===== 評価 ============================================================= */
typedef jsv (*js_native)(jsv self, jsv *args, int nargs);
static struct { js_native fn; } js_nat[64]; static int js_nat_n;
static jsv js_make_nat(js_native f)
{ if (js_nat_n >= 64) { js_fail("組込みがあふれました"); return JU(); }
  jsv v = JU(); v.t = JT_NAT; v.o = js_nat_n; js_nat[js_nat_n++].fn = f; return v; }

static int js_ret_flag; static jsv js_ret_val;
static jsv js_eval(int n, int env);

static jsv js_call(jsv f, jsv self, jsv *args, int nargs)
{
    if (f.t == JT_NAT) return js_nat[f.o].fn(self, args, nargs);
    if (f.t != JT_FUN) { js_fail("関数でないものを呼びました"); return JU(); }
    int node = js_prop_n; (void)node;
    int fnode = (int)js_get_prop(f.o, js_cstr("__node")).n;
    int fenv  = (int)js_get_prop(f.o, js_cstr("__env")).n;
    int e = js_new_env(fenv);
    { int p = js_nd[fnode].a, i = 0;
      while (p >= 0) { js_declare(e, js_nd[p].str, i < nargs ? args[i] : JU()); i++; p = js_nd[p].d; } }
    js_declare(e, js_cstr("this"), self);
    js_ret_flag = 0;
    js_eval(js_nd[fnode].b, e);
    jsv r = js_ret_flag ? js_ret_val : JU();
    js_ret_flag = 0;
    return r;
}

/* DOM とホストは js_dom.c 側で用意する（この評価器は知らなくてよい）。 */
extern jsv js_host_global(int name);            /* 大域名の解決。未知なら UNDEF */
extern jsv js_host_member(jsv obj, int key);    /* ホスト側オブジェクトの参照 */
extern int js_host_setmember(jsv obj, int key, jsv val);  /* 1 なら処理済み */

static jsv js_eval(int n, int env)
{
    if (n < 0 || js_has_err) return JU();
    jnode *d = &js_nd[n];
    switch (d->k) {
    case N_NUM:   return JN(d->num);
    case N_STR:   return JS_(d->str);
    case N_BOOL:  return JB((int)d->num);
    case N_NULL:  { jsv v = JU(); v.t = JT_NULL; return v; }
    case N_UNDEF: return JU();
    case N_NAME:  { jsv *p = js_lookup(env, d->str); if (p) return *p;
                    return js_host_global(d->str); }
    case N_OBJ:   { int o = js_new_obj(0);
                    for (int c = d->a; c >= 0; c = js_nd[c].d)
                        js_set_prop(o, js_nd[c].str, js_eval(js_nd[c].a, env));
                    jsv v = JU(); v.t = JT_OBJ; v.o = o; return v; }
    case N_ARR:   { int o = js_new_obj(1); long i = 0;
                    for (int c = d->a; c >= 0; c = js_nd[c].d)
                        js_set_prop(o, js_numstr(i++), js_eval(js_nd[c].a, env));
                    js_set_prop(o, js_cstr("length"), JN(i));
                    jsv v = JU(); v.t = JT_OBJ; v.o = o; return v; }
    case N_FUN: case N_ARROW: {
        int o = js_new_obj(0);
        js_set_prop(o, js_cstr("__node"), JN(n));
        js_set_prop(o, js_cstr("__env"),  JN(env));
        jsv v = JU(); v.t = JT_FUN; v.o = o;
        if (d->k == N_FUN && d->str) js_declare(env, d->str, v);
        return v; }
    case N_UN: {
        if (js_seq(d->str, "typeof")) {
            jsv a = js_eval(d->a, env);
            const char *t = a.t==JT_STR?"string": a.t==JT_NUM?"number": a.t==JT_BOOL?"boolean":
                            a.t==JT_FUN||a.t==JT_NAT?"function": a.t==JT_UNDEF?"undefined":"object";
            return JS_(js_cstr(t)); }
        jsv a = js_eval(d->a, env);
        if (js_seq(d->str, "!")) return JB(!js_truthy(a));
        if (js_seq(d->str, "-")) return JN(-a.n);
        return JU(); }
    case N_BIN: {
        if (js_seq(d->str,"&&")) { jsv a = js_eval(d->a, env); return js_truthy(a) ? js_eval(d->b, env) : a; }
        if (js_seq(d->str,"||")) { jsv a = js_eval(d->a, env); return js_truthy(a) ? a : js_eval(d->b, env); }
        jsv a = js_eval(d->a, env), b = js_eval(d->b, env);
        if (js_seq(d->str,"+")) {
            if (a.t==JT_STR || b.t==JT_STR) return JS_(js_str_concat(js_tostr(a), js_tostr(b)));
            return JN(a.n + b.n); }
        if (js_seq(d->str,"-")) return JN(a.n - b.n);
        if (js_seq(d->str,"*")) return JN(a.n * b.n);
        if (js_seq(d->str,"/")) return JN(b.n ? a.n / b.n : 0);
        if (js_seq(d->str,"%")) return JN(b.n ? a.n % b.n : 0);
        if (js_seq(d->str,"<")) return JB(a.n <  b.n);
        if (js_seq(d->str,">")) return JB(a.n >  b.n);
        if (js_seq(d->str,"<=")) return JB(a.n <= b.n);
        if (js_seq(d->str,">=")) return JB(a.n >= b.n);
        if (js_seq(d->str,"===")||js_seq(d->str,"==")) return JB(js_eqv(a,b));
        if (js_seq(d->str,"!==")||js_seq(d->str,"!=")) return JB(!js_eqv(a,b));
        return JU(); }
    case N_COND: return js_truthy(js_eval(d->a, env)) ? js_eval(d->b, env) : js_eval(d->c, env);
    case N_MEMBER: {
        jsv o = js_eval(d->a, env);
        if (o.t == JT_OBJ) { jsv h = js_host_member(o, d->str); if (h.t != JT_UNDEF) return h;
                             return js_get_prop(o.o, d->str); }
        return js_host_member(o, d->str); }
    case N_INDEX: {
        jsv o = js_eval(d->a, env), k = js_eval(d->b, env);
        int key = js_tostr(k);
        if (o.t == JT_OBJ) { jsv h = js_host_member(o, key); if (h.t != JT_UNDEF) return h;
                             return js_get_prop(o.o, key); }
        return js_host_member(o, key); }
    case N_ASSIGN: {
        jsv v = js_eval(d->b, env);
        jnode *t = &js_nd[d->a];
        if (js_seq(d->str,"+=")) { jsv cur = js_eval(d->a, env);
            v = (cur.t==JT_STR||v.t==JT_STR) ? JS_(js_str_concat(js_tostr(cur), js_tostr(v)))
                                             : JN(cur.n + v.n); }
        if (t->k == N_NAME) { jsv *p = js_lookup(env, t->str);
                              if (p) *p = v; else js_declare(env, t->str, v); return v; }
        if (t->k == N_MEMBER) { jsv o = js_eval(t->a, env);
            if (js_host_setmember(o, t->str, v)) return v;
            if (o.t == JT_OBJ) js_set_prop(o.o, t->str, v); return v; }
        if (t->k == N_INDEX) { jsv o = js_eval(t->a, env); int key = js_tostr(js_eval(t->b, env));
            if (js_host_setmember(o, key, v)) return v;
            if (o.t == JT_OBJ) js_set_prop(o.o, key, v); return v; }
        js_fail("代入先が式です"); return v; }
    case N_CALL: {
        jsv self = JU(), f;
        jnode *c = &js_nd[d->a];
        if (c->k == N_MEMBER)      { self = js_eval(c->a, env); f = js_host_member(self, c->str);
                                     if (f.t == JT_UNDEF && self.t == JT_OBJ) f = js_get_prop(self.o, c->str); }
        else if (c->k == N_INDEX)  { self = js_eval(c->a, env); int k = js_tostr(js_eval(c->b, env));
                                     f = js_host_member(self, k);
                                     if (f.t == JT_UNDEF && self.t == JT_OBJ) f = js_get_prop(self.o, k); }
        else                        f = js_eval(d->a, env);
        jsv args[8]; int na = 0;
        for (int p = d->b; p >= 0 && na < 8; p = js_nd[p].d) args[na++] = js_eval(js_nd[p].a, env);
        return js_call(f, self, args, na); }
    case N_BLOCK: { int e = js_new_env(env);
                    for (int c = d->a; c >= 0 && !js_ret_flag && !js_has_err; c = js_nd[c].d)
                        js_eval(js_nd[c].a, e);
                    return JU(); }
    case N_VAR:   { for (int c = d->a; c >= 0; c = js_nd[c].d)
                        js_declare(env, js_nd[c].str, js_nd[c].a >= 0 ? js_eval(js_nd[c].a, env) : JU());
                    return JU(); }
    case N_EXPR:  return js_eval(d->a, env);
    case N_RETURN:{ js_ret_val = d->a >= 0 ? js_eval(d->a, env) : JU(); js_ret_flag = 1; return js_ret_val; }
    case N_IF:    { if (js_truthy(js_eval(d->a, env))) js_eval(d->b, env);
                    else if (d->c >= 0) js_eval(d->c, env); return JU(); }
    case N_WHILE: { long guard = 0;
                    while (js_truthy(js_eval(d->a, env)) && !js_ret_flag && !js_has_err) {
                        if (++guard > 2000000L) { js_fail("while が終わりません"); break; }
                        js_eval(d->b, env); }
                    return JU(); }
    case N_FOR:   { int e = js_new_env(env); long guard = 0;
                    if (d->a >= 0) js_eval(d->a, e);
                    while ((d->b < 0 || js_truthy(js_eval(d->b, e))) && !js_ret_flag && !js_has_err) {
                        if (++guard > 2000000L) { js_fail("for が終わりません"); break; }
                        js_eval(d->d, e);
                        if (d->c >= 0) js_eval(d->c, e); }
                    return JU(); }
    default: return JU(); }
}

/* ===== 入口 ============================================================= */
static int js_global_env;

void js_reset(void)
{
    js_str_n = 1; js_str[0] = 0;
    for (int i = 0; i < JS_HASH; i++) js_hash_head[i] = -1;
    js_hash_n = 0;
    js_nd_n = 0; js_obj_n = 0; js_prop_n = 0; js_env_n = 0; js_var_n = 0; js_nat_n = 0;
    js_has_err = 0; js_err[0] = 0; js_ret_flag = 0;
    js_global_env = js_new_env(-1);
}
int js_global(void) { return js_global_env; }
const char *js_error(void) { return js_err; }
int js_failed(void) { return js_has_err; }
int js_error_pos(void) { return js_err_pos; }
/* 失敗箇所の前後を返す ―― 「記号が合いません」だけでは直せない。 */
int js_error_snippet(char *d, int cap)
{
    int o = 0;
    if (js_err_pos < 0 || !js_src) { d[0] = 0; return 0; }
    int a = js_err_pos - 40; if (a < 0) a = 0;
    int b = js_err_pos + 40; if (b > js_srclen) b = js_srclen;
    for (int i = a; i < b && o < cap - 6; i++) {
        if (i == js_err_pos) { d[o++]='>'; d[o++]='>'; }
        char c = js_src[i];
        d[o++] = (c=='\n'||c=='\r'||c=='\t') ? ' ' : c;
    }
    d[o] = 0; return o;
}

/* ソースを評価し、最後の式の値を文字列で返す。 */
/* 領域の使用量。あふれる前に見えるようにしておく。 */
int js_usage(char *d, int cap)
{
    int o = 0;
    #define PU(lbl,v,mx) do { const char *q=lbl; while(*q&&o<cap-1) d[o++]=*q++; \
        { long x=(v); char b[16]; int m=0; if(!x)b[m++]='0'; while(x){b[m++]=(char)('0'+x%10);x/=10;} \
          while(m&&o<cap-1) d[o++]=b[--m]; } if(o<cap-1) d[o++]='/'; \
        { long x=(mx); char b[16]; int m=0; if(!x)b[m++]='0'; while(x){b[m++]=(char)('0'+x%10);x/=10;} \
          while(m&&o<cap-1) d[o++]=b[--m]; } if(o<cap-1) d[o++]=' '; } while(0)
    PU("str=",  js_str_n,  JS_STRCAP);
    PU("node=", js_nd_n,   JS_NODECAP);
    PU("obj=",  js_obj_n,  JS_OBJCAP);
    PU("prop=", js_prop_n, JS_PROPCAP);
    PU("var=",  js_var_n,  JS_VARCAP);
    PU("env=",  js_env_n,  JS_ENVCAP);
    #undef PU
    d[o] = 0; return o;
}

int js_run(const char *src, int len)
{
    js_src = src; js_srclen = len; js_pos = 0;
    js_next();
    jsv last = JU();
    while (js_tk.k != T_EOF && !js_has_err) {
        int s = js_parse_stmt();
        last = js_eval(s, js_global_env);
        if (js_ret_flag) break;
    }
    return js_tostr(last);
}

/* ===== ホストと DOM ====================================================
 * 目的は汎用の DOM ではなく、**このページを組み立てられること**である。
 * 対象が使うのは querySelectorAll / forEach / getAttribute / innerHTML /
 * localStorage / documentElement.lang / addEventListener の 7 つだけ
 * （数えて確かめた）。
 *
 * 節点は「HTML の中の区間」として持つ。DOM 木を作らず、
 * 要素の内側の範囲を覚えておいて、innerHTML への代入をその区間の
 * 差し替えとして記録する。最後に元の HTML を差し替えつきで書き出す。
 * 木を作らないので入れ子の扱いは弱いが、data-i18n 要素は葉に近く、
 * 対象のページはこれで足りる。足りなくなったら木にする。 */
#define DOM_MAXNODE 256
static struct { int tag_off; int inner_a, inner_b; int attr_a, attr_b; int repl; } dom[DOM_MAXNODE];
static int dom_n;
static const char *dom_html; static int dom_html_len;

static char js_out[4096]; static int js_out_n;
const char *js_output(void) { js_out[js_out_n] = 0; return js_out; }
void js_output_clear(void) { js_out_n = 0; js_out[0] = 0; }
static void js_emit(const char *s){ while (*s && js_out_n < 4090) js_out[js_out_n++] = *s++; }

static int dom_eqn(const char *a, const char *b, int n)
{ for (int i=0;i<n;i++) if (a[i]!=b[i]) return 0; return 1; }

/* data-i18n を持つ要素を拾う。開きタグの属性範囲と内側の範囲を覚える。 */
void js_dom_load(const char *html, int len)
{
    dom_html = html; dom_html_len = len; dom_n = 0;
    const char *key = "data-i18n=\"";
    int kl = 11;
    for (int i = 0; i + kl < len && dom_n < DOM_MAXNODE; i++) {
        if (!dom_eqn(html + i, key, kl)) continue;
        int lt = i; while (lt > 0 && html[lt] != '<') lt--;      /* 開きタグの頭 */
        if (html[lt] != '<') continue;
        int ts = lt + 1, te = ts;
        while (te < len && html[te] != ' ' && html[te] != '>' && html[te] != '/') te++;
        int gt = te; while (gt < len && html[gt] != '>') gt++;   /* 開きタグの終わり */
        if (gt >= len) continue;
        /* 同名タグの入れ子を数えて閉じを探す */
        int depth = 1, p = gt + 1, inner_a = gt + 1, inner_b = -1;
        while (p + 1 < len && depth) {
            if (html[p] == '<') {
                if (html[p+1] == '/') {
                    if (dom_eqn(html + p + 2, html + ts, te - ts)) { depth--; if (!depth) inner_b = p; }
                } else if (dom_eqn(html + p + 1, html + ts, te - ts)) depth++;
            }
            p++;
        }
        if (inner_b < 0) inner_b = inner_a;
        dom[dom_n].tag_off = ts; dom[dom_n].attr_a = te; dom[dom_n].attr_b = gt;
        dom[dom_n].inner_a = inner_a; dom[dom_n].inner_b = inner_b;
        dom[dom_n].repl = -1;
        dom_n++;
        i = gt;
    }
}
int js_dom_count(void) { return dom_n; }

/* 差し替えを反映して書き出す。 */
int js_dom_render(char *dst, int cap)
{
    int o = 0, p = 0;
    for (int k = 0; k < dom_n && o < cap - 1; k++) {
        if (dom[k].repl < 0) continue;
        for (; p < dom[k].inner_a && o < cap-1; p++) dst[o++] = dom_html[p];
        const char *r = js_str + dom[k].repl;
        while (*r && o < cap-1) dst[o++] = *r++;
        p = dom[k].inner_b;
    }
    for (; p < dom_html_len && o < cap-1; p++) dst[o++] = dom_html[p];
    dst[o] = 0; return o;
}

/* ---- 組込み関数 ---- */
static jsv js_nat_log(jsv self, jsv *a, int n)
{ (void)self; for (int i = 0; i < n; i++) { if (i) js_emit(" "); js_emit(js_str + js_tostr(a[i])); }
  js_emit("\n"); return JU(); }

static jsv js_mkobj(int cls){ jsv v = JU(); v.t = JT_OBJ; v.o = js_new_obj(cls); return v; }

/* querySelectorAll: 対応するのは属性セレクタ [name] のみ。 */
static jsv js_nat_qsa(jsv self, jsv *a, int n)
{
    (void)self;
    jsv list = js_mkobj(5);                     /* 5 = NodeList */
    long cnt = 0;
    if (n >= 1 && a[0].t == JT_STR) {
        const char *sel = js_str + a[0].s;
        if (sel[0] == '[') {                    /* [data-i18n] だけを見る */
            for (int k = 0; k < dom_n; k++) {
                jsv node = js_mkobj(2);         /* 2 = 要素 */
                js_set_prop(node.o, js_cstr("__i"), JN(k));
                js_set_prop(list.o, js_numstr(cnt++), node);
            }
        }
    }
    js_set_prop(list.o, js_cstr("length"), JN(cnt));
    return list;
}
static jsv js_nat_foreach(jsv self, jsv *a, int n)
{
    if (n < 1 || self.t != JT_OBJ) return JU();
    long len = js_get_prop(self.o, js_cstr("length")).n;
    for (long i = 0; i < len && !js_has_err; i++) {
        jsv el = js_get_prop(self.o, js_numstr(i));
        jsv args[2]; args[0] = el; args[1] = JN(i);
        js_call(a[0], JU(), args, 2);
    }
    return JU();
}
static jsv js_nat_getattr(jsv self, jsv *a, int n)
{
    if (n < 1 || self.t != JT_OBJ) return JU();
    int k = (int)js_get_prop(self.o, js_cstr("__i")).n;
    if (k < 0 || k >= dom_n) return JU();
    const char *want = js_str + a[0].s; int wl = 0; while (want[wl]) wl++;
    /* 開きタグの属性範囲から name="value" を拾う */
    for (int p = dom[k].attr_a; p + wl + 2 < dom[k].attr_b; p++) {
        if (!dom_eqn(dom_html + p, want, wl)) continue;
        int q = p + wl; while (q < dom[k].attr_b && dom_html[q] == ' ') q++;
        if (dom_html[q] != '=') continue;
        q++; while (q < dom[k].attr_b && (dom_html[q]==' '||dom_html[q]=='"')) q++;
        int st = q; while (q < dom_html_len && dom_html[q] != '"') q++;
        return JS_(js_intern(dom_html + st, q - st));
    }
    { jsv v = JU(); v.t = JT_NULL; return v; }
}
static jsv js_nat_getitem(jsv self, jsv *a, int n)
{ (void)self; (void)a; (void)n; jsv v = JU(); v.t = JT_NULL; return v; }   /* 常に未設定 */
static jsv js_nat_setitem(jsv self, jsv *a, int n){ (void)self;(void)a;(void)n; return JU(); }
static jsv js_nat_addev(jsv self, jsv *a, int n)
{ /* DOMContentLoaded は「もう読み込み済み」として即座に呼ぶ。 */
  (void)self;
  if (n >= 2 && a[0].t == JT_STR && js_seq(a[0].s, "DOMContentLoaded"))
      js_call(a[1], JU(), 0, 0);
  return JU(); }
static jsv js_nat_noop(jsv self, jsv *a, int n){ (void)self;(void)a;(void)n; return JU(); }
static jsv js_nat_null(jsv self, jsv *a, int n)
{ (void)self;(void)a;(void)n; jsv v = JU(); v.t = JT_NULL; return v; }

/* 配列 includes / indexOf */
static jsv js_nat_includes(jsv self, jsv *a, int n)
{
    if (self.t != JT_OBJ || n < 1) return JB(0);
    long len = js_get_prop(self.o, js_cstr("length")).n;
    for (long i = 0; i < len; i++) if (js_eqv(js_get_prop(self.o, js_numstr(i)), a[0])) return JB(1);
    return JB(0);
}
/* 文字列 toLowerCase / toUpperCase / startsWith */
static jsv js_nat_lower(jsv self, jsv *a, int n)
{ (void)a;(void)n; if (self.t != JT_STR) return self;
  const char *x = js_str + self.s; int L = 0; while (x[L]) L++;
  if (js_str_n + L + 1 >= JS_STRCAP) return self;
  int at = js_str_n;
  for (int i = 0; i < L; i++) { char c = x[i]; js_str[at+i] = (c>='A'&&c<='Z') ? (char)(c+32) : c; }
  js_str[at+L] = 0; js_str_n += L+1; return JS_(at); }
static jsv js_nat_starts(jsv self, jsv *a, int n)
{ if (self.t != JT_STR || n < 1 || a[0].t != JT_STR) return JB(0);
  const char *x = js_str + self.s, *y = js_str + a[0].s;
  int i = 0; while (y[i]) { if (x[i] != y[i]) return JB(0); i++; } return JB(1); }

jsv js_host_global(int name)
{
    if (js_seq(name, "console")) {
        jsv o = js_mkobj(0); js_set_prop(o.o, js_cstr("log"), js_make_nat(js_nat_log)); return o; }
    if (js_seq(name, "document"))     return js_mkobj(3);
    if (js_seq(name, "localStorage")) return js_mkobj(4);
    if (js_seq(name, "window"))       return js_mkobj(6);
    if (js_seq(name, "navigator")) {
        jsv o = js_mkobj(0); js_set_prop(o.o, js_cstr("language"), JS_(js_cstr("en"))); return o; }
    return JU();
}

jsv js_host_member(jsv o, int key)
{
    if (o.t == JT_STR) {
        if (js_seq(key, "length")) { const char *s = js_str + o.s; long n = 0; while (s[n]) n++; return JN(n); }
        if (js_seq(key, "toLowerCase") || js_seq(key, "toUpperCase")) return js_make_nat(js_nat_lower);
        if (js_seq(key, "startsWith")) return js_make_nat(js_nat_starts);
    }
    if (o.t != JT_OBJ) return JU();
    int cls = js_obj[o.o].cls;
    if (cls == 3) {                              /* document */
        if (js_seq(key,"querySelectorAll")||js_seq(key,"querySelector")) return js_make_nat(js_nat_qsa);
        if (js_seq(key,"addEventListener")) return js_make_nat(js_nat_addev);
        if (js_seq(key,"documentElement")) { jsv e = js_mkobj(7); return e; }
        if (js_seq(key,"getElementById")) return js_make_nat(js_nat_null);   /* 無い扱い */
        if (js_seq(key,"body")) return js_mkobj(7);
        return JU();
    }
    if (cls == 4) {                              /* localStorage */
        if (js_seq(key,"getItem")) return js_make_nat(js_nat_getitem);
        if (js_seq(key,"setItem")) return js_make_nat(js_nat_setitem);
        return JU();
    }
    if (cls == 5 || cls == 1) {                  /* NodeList / 配列 */
        if (js_seq(key,"forEach"))  return js_make_nat(js_nat_foreach);
        if (js_seq(key,"includes")) return js_make_nat(js_nat_includes);
        return JU();
    }
    if (cls == 2) {                              /* 要素 */
        if (js_seq(key,"getAttribute")) return js_make_nat(js_nat_getattr);
        if (js_seq(key,"addEventListener")) return js_make_nat(js_nat_noop);
        if (js_seq(key,"innerHTML")||js_seq(key,"textContent")) {
            int k = (int)js_get_prop(o.o, js_cstr("__i")).n;
            if (k >= 0 && k < dom_n)
                return JS_(js_intern(dom_html + dom[k].inner_a, dom[k].inner_b - dom[k].inner_a));
            return JU();
        }
        return JU();
    }
    if (cls == 6 || cls == 7) {                  /* window / documentElement */
        if (js_seq(key,"addEventListener")) return js_make_nat(js_nat_addev);
        return JU();
    }
    return JU();
}

int js_host_setmember(jsv o, int key, jsv v)
{
    if (o.t != JT_OBJ) return 0;
    if (js_obj[o.o].cls == 2 && (js_seq(key,"innerHTML")||js_seq(key,"textContent"))) {
        int k = (int)js_get_prop(o.o, js_cstr("__i")).n;
        if (k >= 0 && k < dom_n) dom[k].repl = js_tostr(v);
        return 1;
    }
    if (js_obj[o.o].cls == 7) return 1;          /* documentElement.lang = ... は捨てる */
    return 0;
}
const char *js_strbase(void) { return js_str; }
