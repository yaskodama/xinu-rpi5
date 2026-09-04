// cc/abcl2c.c — on-device AIPL (.abcl) -> C translator (the xinu-jit subset).
//
// Turns a real AIPL actor program (classes / fields / methods / new / send /
// now / reply / if / while / return / print / arithmetic / strings) into the
// self-contained integer-subset C the on-device compiler (cc) JITs.  Same
// shape as the host `aipl2c --xinu-jit`, but on the device so `cc foo.abcl`
// and `make foo.abcl` run real AIPL with no host round-trip.
//
// Surface syntax follows the canonical AIPL guide (docs/samples/guide/g*.aipl):
//   method m(x: D) : unit !{log, mut} @ 2 { ... }   annotations are read and dropped
//   ++   string concatenation (maps to v_add; the runtime concatenates)
//   true / false                                    bool is int here: 1 / 0
//   reply(e)                                        == `return e` on this device,
//                                                   so it must end its block
//   now t.m(a) timeout <ms> else <e>                deadline, checked AFTER the
//                                                   call: the value matches the
//                                                   canonical one, but the call
//                                                   is never interrupted (the
//                                                   pump is cooperative)
// Not on this device, and each says so by name rather than miscompiling:
//   future / await / select / spawn / remote / replyto / web_*
// Of the 10 canonical guide samples this translates 6 (g1 g3 g6 g8 g9 g10); the
// other 4 need those runtime features.  Measure with the guide samples.
//
// Runtime seam (provided by cc.c via cc_resolve_extern):
//   v_int v_str v_add v_sub v_mul v_div v_lt v_le v_eq v_ne v_and v_or v_not
//   v_truthy v_int_of v_print v_ai_call enqueue cc_actor_new  (dispatch is generated)
// There is no v_gt/v_ge/%: `>`/`>=` swap operands onto v_lt/v_le; `%` errors.
//
// Portable C.  Build with -DABCL2C_HOST_TEST for a standalone translator
// (reads argv[1].abcl, prints C) used to diff against the host tool.

#ifdef ABCL2C_HOST_TEST
#include <stdio.h>
#include <stdlib.h>
#endif

/* ---- error reporting ---------------------------------------------- */
static char a2c_err[160];
const char *abcl2c_error(void) { return a2c_err[0] ? a2c_err : "ok"; }
static int  a2c_fail(const char *m)
{ int i = 0; for (; m[i] && i < (int)sizeof a2c_err - 1; i++) a2c_err[i] = m[i]; a2c_err[i] = 0; return -1; }

/* ---- lexer -------------------------------------------------------- */
enum { T_EOF, T_ID, T_NUM, T_STR, T_PUNCT };
/* 文字列リテラルは日本語だと 1 文字 3 バイトになる。64 では足りず、しかも
   足りないときに閉じ引用符まで読み進めずに戻っていたので、字句解析が文字列の
   途中から再開して壊れた（"expected expression" になる）。長さを増やし、
   それでも溢れるときは名前をつけて断る。 */
#define TOKSTR 160
typedef struct { int kind; char s[TOKSTR]; long num; } tok_t;

static const char *L;          /* scan cursor (just past current token)   */
static const char *Ts;         /* start of current token in the source    */
static tok_t       T;          /* current token                           */

static int sp(char c)  { return c==' '||c=='\t'||c=='\r'||c=='\n'; }
static int dg(char c)  { return c>='0'&&c<='9'; }
static int al(char c)  { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int an(char c)  { return al(c)||dg(c); }

static void lex_next(void)
{
    for (;;) {                                       /* skip ws + comments */
        while (*L && sp(*L)) L++;
        if (L[0]=='/' && L[1]=='/') { L+=2; while (*L && *L!='\n') L++; continue; }
        if (L[0]=='/' && L[1]=='*') { L+=2; while (*L && !(L[0]=='*'&&L[1]=='/')) L++; if (*L) L+=2; continue; }
        break;
    }
    Ts = L; T.kind = T_EOF; T.s[0] = 0; T.num = 0;
    if (!*L) return;
    if (al(*L)) { int n=0; while (an(*L)&&n<TOKSTR-1) T.s[n++]=*L++; T.s[n]=0; T.kind=T_ID; return; }
    if (dg(*L)) { long v=0; while (dg(*L)) v=v*10+(*L++-'0'); if (*L=='.'){L++;while(dg(*L))L++;} T.kind=T_NUM; T.num=v; return; }
    if (*L=='"') {
        L++; int n=0, over=0;
        /* 溢れても閉じ引用符までは必ず読み進める。ここで止まると、この先の
           解析が文字列の中身をトークンとして読み始めてしまう。 */
        while (*L && *L!='"') {
            if (*L=='\\' && L[1]) { if(n<TOKSTR-2){T.s[n++]=*L;}else over=1; L++; }
            if (n<TOKSTR-1) T.s[n++]=*L; else over=1;
            L++;
        }
        if(*L=='"')L++;
        T.s[n]=0; T.kind=T_STR;
        if (over) a2c_fail("string literal too long (max 159 bytes)");
        return; }
    { char a=L[0],b=L[1]; const char *two=0;
      if(a=='='&&b=='=')two="==";else if(a=='!'&&b=='=')two="!=";else if(a=='<'&&b=='=')two="<=";
      else if(a=='>'&&b=='=')two=">=";else if(a=='&'&&b=='&')two="&&";else if(a=='|'&&b=='|')two="||";
      /* ++ は文字列連結。'+' より先に見ないと 1+ +1 に割れる。 */
      else if(a=='+'&&b=='+')two="++";else if(a=='-'&&b=='>')two="->";
      if(two){T.s[0]=two[0];T.s[1]=two[1];T.s[2]=0;L+=2;T.kind=T_PUNCT;return;}
      T.s[0]=a;T.s[1]=0;L++;T.kind=T_PUNCT;return; }
}

typedef struct { const char *L; const char *Ts; tok_t T; } lsave_t;
static lsave_t lex_save(void) { lsave_t s; s.L=L; s.Ts=Ts; s.T=T; return s; }
static void    lex_load(lsave_t s) { L=s.L; Ts=s.Ts; T=s.T; }

static int is_p(const char *p) { return T.kind==T_PUNCT && T.s[0]==p[0] && (p[1]==0 ? T.s[1]==0 : T.s[1]==p[1]); }
static int is_kw(const char *k) { if (T.kind!=T_ID) return 0; int i=0; for(;k[i];i++) if(T.s[i]!=k[i]) return 0; return T.s[i]==0; }
static int a2c_streq(const char *a, const char *b) { int i=0; for(;a[i]&&b[i];i++) if(a[i]!=b[i]) return 0; return a[i]==b[i]; }
static void cpid(char *d) { int i=0; for(;T.s[i]&&i<47;i++) d[i]=T.s[i]; d[i]=0; }
static void cpid_to(char *d){ int i=0; for(;T.s[i]&&i<TOKSTR-1;i++) d[i]=T.s[i]; d[i]=0; }

/* ---- program model ------------------------------------------------ */
#define MAXCLASS 16
#define MAXFIELD 16
#define MAXMETHOD 24
#define MAXPARAM 4
#define MAXMNAME 48
typedef struct {
    char name[48];
    char field[MAXFIELD][48]; long finit[MAXFIELD]; char finit_bool[MAXFIELD]; int nfield;
    struct { char name[48]; char param[MAXPARAM][48]; int nparam; const char *body; } method[MAXMETHOD];
    int nmethod;
} class_t;
static class_t    CLS[MAXCLASS]; static int NCLS;
static char       MNAME[MAXMNAME][48]; static int NMNAME;
static const char *TOPLEVEL;
static int        MAXF;                 /* widest field count -> f[MAXF]      */

static int meth_id(const char *name)
{ for(int i=0;i<NMNAME;i++){int k=0;for(;name[k];k++)if(MNAME[i][k]!=name[k])goto no; if(MNAME[i][k]==0)return i; no:;}
  if(NMNAME<MAXMNAME){int k=0;for(;name[k]&&k<47;k++)MNAME[NMNAME][k]=name[k];MNAME[NMNAME][k]=0;return NMNAME++;} return 0; }
static int class_idx(const char *name)
{ for(int i=0;i<NCLS;i++){int k=0;for(;name[k];k++)if(CLS[i].name[k]!=name[k])goto no; if(CLS[i].name[k]==0)return i; no:;} return -1; }
static int field_idx(class_t *c, const char *name)
{ for(int i=0;i<c->nfield;i++){int k=0;for(;name[k];k++)if(c->field[i][k]!=name[k])goto no; if(c->field[i][k]==0)return i; no:;} return -1; }

/* ---- structural pass ---------------------------------------------- */
static void skip_block(void) {           /* T is '{'; consume to matching '}' */
    int d=0; do { if(is_p("{"))d++; else if(is_p("}"))d--; lex_next(); } while(d>0&&T.kind!=T_EOF);
}

/* 型注釈を読み飛ばす。`unit` `int` `string` `bool` とクラス名、それに `list[int]`
   のような添字つきを受ける。このバックエンドは値をタグ付き 64 ビットで持つので、
   型そのものは使わない。読み飛ばすだけ。 */
static int skip_type(void)
{
    if (T.kind!=T_ID) return a2c_fail("type: expected name");
    lex_next();
    if (is_p("[")) {                      /* list[int] / map[string,int] */
        int d=0, guard=0;
        do { if(is_p("["))d++; else if(is_p("]"))d--; lex_next(); }
        while (d>0 && T.kind!=T_EOF && ++guard<64);
        if (d>0) return a2c_fail("type: unbalanced '['");
    }
    return 0;
}

/* メソッド署名の後置注釈を読み飛ばす。正典 AIPL の
     `method m(x: D) : unit !{log, mut} @ 2 { ... }`
   の `: unit` `!{...}` `@ 2` の部分。従来の `-> T` も受ける。
   ここが無いために、正典のガイド標本 10 本が全部 `method: expected '{'` で
   落ちていた。効果と義務レベルは意味検査が正典側の担当なので、受理して捨てる。 */
static int skip_method_annots(void)
{
    for (int guard=0; guard<16 && T.kind!=T_EOF; guard++) {
        if (is_p("{")) return 0;
        if (is_p(":") || is_p("->")) { lex_next(); if(skip_type()) return -1; continue; }
        if (is_p("!")) {                          /* 効果注釈 !{ ... } */
            lex_next(); if(!is_p("{")) return a2c_fail("effects: expected '{' after '!'");
            int d=0, g2=0;
            do { if(is_p("{"))d++; else if(is_p("}"))d--; lex_next(); }
            while (d>0 && T.kind!=T_EOF && ++g2<128);
            if (d>0) return a2c_fail("effects: unbalanced '{'");
            continue;
        }
        if (is_p("@")) {                          /* 義務レベル @ N */
            lex_next(); if(T.kind!=T_NUM) return a2c_fail("'@': expected level");
            lex_next(); continue;
        }
        break;
    }
    return 0;
}
static int parse_structure(const char *src)
{
    NCLS=0; NMNAME=0; TOPLEVEL=0; MAXF=1; a2c_err[0]=0;
    L=src; lex_next();
    for (;;) {
        if (T.kind==T_EOF) { if(!TOPLEVEL) TOPLEVEL=Ts; break; }
        if (!is_kw("class")) { TOPLEVEL=Ts; break; }
        if (NCLS>=MAXCLASS) return a2c_fail("too many classes");
        class_t *c=&CLS[NCLS++]; c->nfield=0; c->nmethod=0;
        lex_next(); if(T.kind!=T_ID) return a2c_fail("class: expected name"); cpid(c->name); lex_next();
        if(!is_p("{")) return a2c_fail("class: expected '{'");
        lex_next();
        while(!is_p("}") && T.kind!=T_EOF && !a2c_err[0]) {
            if (is_kw("var")) {
                lex_next(); if(T.kind!=T_ID) return a2c_fail("field: expected name");
                if(c->nfield>=MAXFIELD) return a2c_fail("too many fields");
                cpid(c->field[c->nfield]); long init=0; int isb=0; lex_next();
                if(is_p(":")) { lex_next(); if(skip_type()) return -1; }   /* var n: int = 0; */
                if(is_p("=")){
                    lex_next();
                    /* 初期値は long ひとつに入れて g_spawn が v_int で置く。整数と
                       真偽値だけ持てる。以前は「数値でなければ黙って 0」だったので、
                       文字列の初期値が何も言わずに 0 になっていた。名前をつけて断る。 */
                    int neg=0; if(is_p("-")){ neg=1; lex_next(); }
                    if(T.kind==T_NUM)        { init = neg ? -T.num : T.num; lex_next(); }
                    else if(is_kw("true"))   { if(neg) return a2c_fail("field init: '-' on bool"); init=1; isb=1; lex_next(); }
                    else if(is_kw("false"))  { if(neg) return a2c_fail("field init: '-' on bool"); init=0; isb=1; lex_next(); }
                    else return a2c_fail("field init: only int/bool literals on this device");
                }
                c->finit[c->nfield]=init; c->finit_bool[c->nfield]=(char)isb;
                c->nfield++; if(c->nfield>MAXF) MAXF=c->nfield;
                if(is_p(";")) lex_next(); else return a2c_fail("field: expected ';'");
            } else if (is_kw("method")) {
                lex_next(); if(T.kind!=T_ID) return a2c_fail("method: expected name");
                if(c->nmethod>=MAXMETHOD) return a2c_fail("too many methods");
                int mi=c->nmethod++; cpid(c->method[mi].name); meth_id(c->method[mi].name); c->method[mi].nparam=0;
                lex_next(); if(!is_p("(")) return a2c_fail("method: expected '('"); lex_next();
                /* 仮引数。`x: D` の型注釈を読む。以前は識別子を無条件に拾っていたので、
                   `x: D` の `D` まで 2 つめの仮引数として数えていた。 */
                while(!is_p(")")&&T.kind!=T_EOF&&!a2c_err[0]){
                    if(T.kind!=T_ID) return a2c_fail("method: expected parameter name");
                    if(c->method[mi].nparam>=MAXPARAM) return a2c_fail("too many parameters (max 4)");
                    cpid(c->method[mi].param[c->method[mi].nparam++]); lex_next();
                    if(is_p(":")) { lex_next(); if(skip_type()) return -1; }
                    if(is_p(",")) lex_next(); else break;
                }
                if(!is_p(")")) return a2c_fail("method: expected ')'");
                lex_next();
                if(skip_method_annots()) return -1;
                if(!is_p("{")) return a2c_fail("method: expected '{'");
                c->method[mi].body=Ts;       /* points at the '{' */
                skip_block();
            } else return a2c_fail("class body: expected var/method");
        }
        if(is_p("}")) lex_next();
    }
    return 0;
}

/* トップレベルの var は C の大域変数にする。以前は main のローカルにしていたので、
   メソッドの中から参照すると宣言の無い名前になり、生成 C が機内 cc で
   "undefined variable" になっていた（g3 の `now stock.take(k)` がこれ）。 */
#define MAXTOPV 64
static char TOPV[MAXTOPV][48]; static int NTOPV;
static int is_topvar(const char *n)
{ for(int i=0;i<NTOPV;i++){int k=0;for(;n[k];k++)if(TOPV[i][k]!=n[k])goto no; if(TOPV[i][k]==0)return 1; no:;} return 0; }
static void scan_topvars(void)
{
    NTOPV=0; if(!TOPLEVEL) return;
    lsave_t sv=lex_save();
    L=TOPLEVEL; lex_next();
    int depth=0, guard=0;
    while (T.kind!=T_EOF && ++guard<100000) {
        if (is_p("{")) depth++;
        else if (is_p("}")) { if(depth>0) depth--; }
        else if (depth==0 && is_kw("var")) {
            lex_next();
            if (T.kind==T_ID && !is_topvar(T.s) && NTOPV<MAXTOPV) {
                int k=0; for(;T.s[k]&&k<47;k++) TOPV[NTOPV][k]=T.s[k]; TOPV[NTOPV][k]=0; NTOPV++;
            }
            continue;                    /* 名前の分は下の lex_next で進む */
        }
        lex_next();
    }
    lex_load(sv);
}

/* ---- select（Pi 3 と同じ「フラグ＋横取り」方式）------------------------
 * Pi 3 は select を待たない。次のように落としている:
 *   ・select のある f は  __sel = sid  を立てて終わる
 *   ・case job(k) -> 本体 は、メソッド job の先頭へ差し込む:
 *       if (__sel == sid) { __sel = 0; 本体; f の残り } else { job の元の本体 }
 *   ・timeout は、対応するメッセージが来なかったときに走る
 * こうすると select を満たすメッセージは通常のメソッド呼び出しで届くので、
 * サスペンドもメールボックス走査も要らない。Pi 5 でも同じ形にする。 */
#define MAXSEL 8
typedef struct {
    int  cls;                  /* このクラスの                     */
    int  owner;                /* このメソッドが持つ select        */
    int  sid;                  /* 1 から振る識別子                 */
    int  ncase;
    int  cmid[4];              /* case が待つメソッド番号          */
    char cvar[4][48];          /* case の仮引数名（1つまで）       */
    const char *cbody[4];      /* case 本体（'{' を指す）          */
    const char *tbody;         /* timeout 本体（無ければ 0）       */
    const char *rest;          /* select の後ろに続く文の先頭      */
} sel_t;
static sel_t SEL[MAXSEL]; static int NSEL;
static int  SELFIELD[MAXCLASS];        /* __sel の隠しフィールド番号。-1 = 無し */

/* ---- expression AST ----------------------------------------------- */
enum { N_NIL, N_INT, N_BOOL, N_STR, N_ID, N_NEW, N_NOW, N_NOWDL, N_FUT, N_AWAIT, N_AWAITDL, N_PRINT, N_AICALL, N_WEB, N_WAIT, N_BIN, N_UN };
enum { O_ADD,O_SUB,O_MUL,O_DIV,O_LT,O_LE,O_GT,O_GE,O_EQ,O_NE,O_AND,O_OR,O_NOT,O_NEG };
typedef struct { int k; long num; char s[TOKSTR]; int op; int a,b; int ci,mid; int args[4]; int nargs; } enode_t;
#define MAXNODE 1024
static enode_t ND[MAXNODE]; static int NND;
static int mknode(int k){ if(NND>=MAXNODE) { a2c_fail("expression too large"); return 0; } int i=NND++; ND[i].k=k; ND[i].nargs=0; return i; }

/* current emit context */
static int   CC;                         /* current class idx, -1 = top level  */
static int   CURMETH = -1;               /* 生成中のメソッド番号（select 用）  */
static char  LOCAL[64][48]; static int NLOCAL;
static int   is_local(const char *n){ for(int i=0;i<NLOCAL;i++){int k=0;for(;n[k];k++)if(LOCAL[i][k]!=n[k])goto no; if(LOCAL[i][k]==0)return 1; no:;} return 0; }
static void  add_local(const char *n){ if(NLOCAL<64){int k=0;for(;n[k]&&k<47;k++)LOCAL[NLOCAL][k]=n[k];LOCAL[NLOCAL][k]=0;NLOCAL++;} }

static int parse_expr(void);
static int parse_unary(void);
static int parse_primary(void)
{
    /* 正典 AIPL にあって、この機内前段が持たない構文。識別子として素通しすると
       壊れた C（v_timeout / v_replyto など）が出て cc 側で分かりにくく落ちる。
       ここで名前を挙げて断る。読み進まないまま上位が回ることも防げる。 */
    if (T.kind==T_ID) {
        static const char *ni[] = { "reply","replyto",
                                    "timeout","spawn","remote", 0 };
        for (int i=0; ni[i]; i++) if (a2c_streq(T.s, ni[i])) {
            char m[80]; int k=0; const char *pre="not supported on this device: ";
            while(pre[k] && k<(int)sizeof m-1){ m[k]=pre[k]; k++; }
            for(int j=0; ni[i][j] && k<(int)sizeof m-1; j++) m[k++]=ni[i][j];
            m[k]=0; a2c_fail(m); return 0;
        }
    }

    if (T.kind==T_NUM) { int n=mknode(N_INT); ND[n].num=T.num; lex_next(); return n; }
    if (T.kind==T_STR) { int n=mknode(N_STR); cpid_to(ND[n].s); lex_next(); return n; }
    /* 真偽値リテラル。このバックエンドは bool を int で持つので 1 / 0 に写す。
       以前はキーワードでなかったため識別子として素通しし、未定義の v_true を
       参照する C が出ていた（実機の cc が "undefined variable" で落ちる）。 */
    if (is_kw("true"))  { int n=mknode(N_BOOL); ND[n].num=1; lex_next(); return n; }
    if (is_kw("false")) { int n=mknode(N_BOOL); ND[n].num=0; lex_next(); return n; }
    if (is_kw("new")) { lex_next(); if(T.kind!=T_ID){a2c_fail("new: expected class");return 0;}
        int ci=class_idx(T.s);
        /* 知らないクラス名だと class_idx が -1 を返す。以前はそれをそのまま
           構文木に入れていたので、生成のときに CLS[-1] を読んでいた。
           ホストでは ASan が捕まえるが、実機ではカーネルの領域外参照になる。 */
        if (ci < 0) {
            char m[96]; int k=0; const char *pre="new: unknown class: ";
            while(pre[k] && k<(int)sizeof m-1){ m[k]=pre[k]; k++; }
            for(int j=0; T.s[j] && k<(int)sizeof m-1; j++) m[k++]=T.s[j];
            m[k]=0; a2c_fail(m); return 0;
        }
        int n=mknode(N_NEW); ND[n].ci=ci; lex_next();
        /* 引数を読み飛ばさずに構文木へ入れる。以前は '(' の次が ')' でないと
           何も進めずに戻っていたので、new A("x") で呼び出し側が無限ループした
           （実機ではカーネルごと固まる）。init があればそれに渡す。 */
        if(is_p("(")){
            lex_next();
            while(!is_p(")")&&T.kind!=T_EOF&&!a2c_err[0]){
                if(ND[n].nargs<4) ND[n].args[ND[n].nargs++]=parse_expr(); else parse_expr();
                if(is_p(",")) lex_next();
            }
            if(is_p(")")) lex_next(); else return (a2c_fail("new: expected ')'"), 0);
        }
        return n; }
    if (is_kw("future")) { lex_next(); int obj=parse_primary();
        if(!is_p(".")){a2c_fail("future: expected .method");return 0;} lex_next();
        if(T.kind!=T_ID){a2c_fail("future: expected method");return 0;}
        int n=mknode(N_FUT); ND[n].mid=meth_id(T.s); ND[n].a=obj; lex_next();
        if(is_p("(")){lex_next(); while(!is_p(")")&&T.kind!=T_EOF&&!a2c_err[0]){ if(ND[n].nargs<4) ND[n].args[ND[n].nargs++]=parse_expr(); else parse_expr(); if(is_p(","))lex_next(); } if(is_p(")"))lex_next();}
        return n; }
    if (is_kw("await")) { lex_next();
        int n=mknode(N_AWAIT); ND[n].a=parse_unary();
        if (is_kw("timeout")) {                       /* await f timeout <ms> else <式> */
            lex_next();
            if(T.kind!=T_NUM) { a2c_fail("timeout: expected milliseconds"); return 0; }
            ND[n].num = T.num; lex_next();
            if(!is_kw("else")) { a2c_fail("timeout: expected 'else'"); return 0; }
            lex_next(); ND[n].b = parse_expr(); ND[n].k = N_AWAITDL;
        }
        return n; }
    if (is_kw("now")) { lex_next(); int obj=parse_primary();
        if(!is_p(".")){a2c_fail("now: expected .method");return 0;} lex_next();
        if(T.kind!=T_ID){a2c_fail("now: expected method");return 0;}
        int n=mknode(N_NOW); ND[n].mid=meth_id(T.s); ND[n].a=obj; lex_next();
        if(is_p("(")){lex_next(); while(!is_p(")")&&T.kind!=T_EOF&&!a2c_err[0]){ if(ND[n].nargs<4) ND[n].args[ND[n].nargs++]=parse_expr(); else parse_expr(); if(is_p(","))lex_next(); } if(is_p(")"))lex_next();}
        /* 正典 AIPL の後置の期限:  now t.m(a) timeout <ms> else <式>
           この装置の実行は協調ポンプで、dispatch は同期的に走りきる。
           だから期限は「超えたか」の事後判定にしか使えない。返る値は
           正典と同じ（超えたら else 節）だが、呼び出しは中断されない。 */
        if (is_kw("timeout")) {
            lex_next();
            if(T.kind!=T_NUM) { a2c_fail("timeout: expected milliseconds"); return 0; }
            ND[n].num = T.num; lex_next();
            if(!is_kw("else")) { a2c_fail("timeout: expected 'else'"); return 0; }
            lex_next();
            ND[n].b = parse_expr();
            ND[n].k = N_NOWDL;
        }
        return n; }
    if (is_kw("self")) { int n=mknode(N_ID); ND[n].s[0]='s';ND[n].s[1]='e';ND[n].s[2]='l';ND[n].s[3]='f';ND[n].s[4]=0; lex_next(); return n; }
    if (is_p("(")) { lex_next(); int e=parse_expr(); if(is_p(")"))lex_next(); return e; }
    if (T.kind==T_ID) {
        char nm[48]; cpid(nm); lex_next();
        if (is_p("(")) {
            /* 組込み呼び出し。以前はどんな名前でも v_print に落としていたので、
               ai_call(x) や sqrt(x) が黙って print(x) になっていた。
               受けるものだけを受け、それ以外はその場で失敗させる。 */
            int isprint = (a2c_streq(nm,"print") || a2c_streq(nm,"puts"));
            int isai    = a2c_streq(nm,"ai_call");
            /* wait(ms) — 別プロセス経路（シェルの cc/make）でだけ本物の休眠。
               インライン経路（/cc, /actor/load）では板が固まるのでランタイムが断る。 */
            int iswait  = a2c_streq(nm,"wait");
            /* web_listen(port) / web_expose(path, "名前")
               ポート指定はこの装置では無視され、公開ルートは既存の 80 番の
               サーバに載る。web_expose の第2引数はトップレベルの var 名で、
               板はアクター番号で持つので、ここで名前を解決して番号を渡す。 */
            int isweb   = (a2c_streq(nm,"web_listen") || a2c_streq(nm,"web_expose"));
            if (isweb) {
                int n=mknode(N_WEB); ND[n].num = a2c_streq(nm,"web_expose"); lex_next();
                while(!is_p(")")&&T.kind!=T_EOF&&!a2c_err[0]){
                    if(ND[n].nargs<4) ND[n].args[ND[n].nargs++]=parse_expr(); else parse_expr();
                    if(is_p(","))lex_next(); }
                if(is_p(")"))lex_next();
                if (ND[n].num) {                      /* web_expose */
                    if (ND[n].nargs != 2) { a2c_fail("web_expose: expected (path, \"actor\")"); return 0; }
                    int an = ND[n].args[1];
                    if (ND[an].k != N_STR || !is_topvar(ND[an].s)) {
                        char m[96]; int k=0; const char *pre="web_expose: unknown actor: ";
                        while(pre[k] && k<(int)sizeof m-1){ m[k]=pre[k]; k++; }
                        for(int j=0; ND[an].s[j] && k<(int)sizeof m-1; j++) m[k++]=ND[an].s[j];
                        m[k]=0; a2c_fail(m); return 0;
                    }
                } else if (ND[n].nargs != 1) { a2c_fail("web_listen: expected (port)"); return 0; }
                return n;
            }
            if (iswait) {
                int n=mknode(N_WAIT); lex_next();
                while(!is_p(")")&&T.kind!=T_EOF&&!a2c_err[0]){
                    if(ND[n].nargs<4) ND[n].args[ND[n].nargs++]=parse_expr(); else parse_expr();
                    if(is_p(","))lex_next(); }
                if(is_p(")"))lex_next();
                if(ND[n].nargs != 1) { a2c_fail("wait: expected (milliseconds)"); return 0; }
                return n;
            }
            if (!isprint && !isai) {
                char m[96]; int k=0;
                const char *pre="unknown function: ";
                while(pre[k] && k<(int)sizeof m-1){ m[k]=pre[k]; k++; }
                for(int j=0; nm[j] && k<(int)sizeof m-1; j++) m[k++]=nm[j];
                m[k]=0; a2c_fail(m); return 0;
            }
            int n=mknode(isai ? N_AICALL : N_PRINT); lex_next();
            while(!is_p(")")&&T.kind!=T_EOF&&!a2c_err[0]){ if(ND[n].nargs<4) ND[n].args[ND[n].nargs++]=parse_expr(); else parse_expr(); if(is_p(","))lex_next(); }
            if(is_p(")"))lex_next();
            return n;
        }
        int n=mknode(N_ID); { int k=0; for(;nm[k]&&k<47;k++) ND[n].s[k]=nm[k]; ND[n].s[k]=0; } return n;
    }
    a2c_fail("expected expression"); return 0;
}
static int parse_unary(void)
{
    if (is_p("!")) { lex_next(); int n=mknode(N_UN); ND[n].op=O_NOT; ND[n].a=parse_unary(); return n; }
    if (is_p("-")) { lex_next(); int n=mknode(N_UN); ND[n].op=O_NEG; ND[n].a=parse_unary(); return n; }
    return parse_primary();
}
static int bin(int op,int a,int b){ int n=mknode(N_BIN); ND[n].op=op; ND[n].a=a; ND[n].b=b; return n; }
static int parse_mul(void){ int a=parse_unary(); for(;;){ int op; if(is_p("*"))op=O_MUL; else if(is_p("/"))op=O_DIV; else if(is_p("%")){a2c_fail("'%' not supported");return 0;} else break; lex_next(); a=bin(op,a,parse_unary()); } return a; }
static int parse_add(void){ int a=parse_mul(); for(;;){ int op; if(is_p("+"))op=O_ADD; else if(is_p("-"))op=O_SUB; else break; lex_next(); a=bin(op,a,parse_mul()); } return a; }
/* 文字列連結。優先順位は 等値 < 比較 < ++ < 加減。ランタイムの v_add は
   片方が文字列なら連結するので、O_ADD にそのまま写せる（cc.c:365）。 */
static int parse_concat(void){ int a=parse_add(); while(is_p("++")){ lex_next(); a=bin(O_ADD,a,parse_add()); } return a; }
static int parse_rel(void){ int a=parse_concat(); for(;;){ int op; if(is_p("<="))op=O_LE; else if(is_p(">="))op=O_GE; else if(is_p("<"))op=O_LT; else if(is_p(">"))op=O_GT; else break; lex_next(); a=bin(op,a,parse_concat()); } return a; }
static int parse_eql(void){ int a=parse_rel(); for(;;){ int op; if(is_p("=="))op=O_EQ; else if(is_p("!="))op=O_NE; else break; lex_next(); a=bin(op,a,parse_rel()); } return a; }
static int parse_and(void){ int a=parse_eql(); while(is_p("&&")){ lex_next(); a=bin(O_AND,a,parse_eql()); } return a; }
static int parse_or (void){ int a=parse_and(); while(is_p("||")){ lex_next(); a=bin(O_OR,a,parse_and()); } return a; }
static int parse_expr(void){ return parse_or(); }

/* 各メソッドの本体を走査して select を拾い、SEL[] に積む。
   併せて、select を使うクラスへ隠しフィールド __sel を足す。
   本体は原文へのポインタで持っているので、ここでも lex し直して位置を控える。 */
static int scan_selects(void)
{
    NSEL = 0;
    for (int i=0;i<MAXCLASS;i++) SELFIELD[i] = -1;
    lsave_t sv = lex_save();
    for (int ci=0; ci<NCLS; ci++) {
        for (int mi=0; mi<CLS[ci].nmethod; mi++) {
            L = CLS[ci].method[mi].body; lex_next();      /* '{' から */
            if (!is_p("{")) continue;
            lex_next();
            int depth = 0;
            while (T.kind!=T_EOF) {
                if (is_p("{")) { depth++; lex_next(); continue; }
                if (is_p("}")) { if (depth==0) break; depth--; lex_next(); continue; }
                if (depth==0 && is_kw("select")) {
                    if (NSEL>=MAXSEL) { lex_load(sv); return a2c_fail("too many selects"); }
                    sel_t *sp = &SEL[NSEL]; sp->cls=ci; sp->owner=mi; sp->sid=NSEL+1;
                    sp->ncase=0; sp->tbody=0; sp->rest=0;
                    lex_next();
                    if(!is_p("{")) { lex_load(sv); return a2c_fail("select: expected '{'"); }
                    lex_next();
                    while (!is_p("}") && T.kind!=T_EOF) {
                        if (is_kw("case")) {
                            lex_next();
                            if(T.kind!=T_ID) { lex_load(sv); return a2c_fail("select: expected a message name"); }
                            if(sp->ncase>=4) { lex_load(sv); return a2c_fail("select: too many cases (max 4)"); }
                            int k = sp->ncase;
                            sp->cmid[k]=meth_id(T.s); sp->cvar[k][0]=0; lex_next();
                            if(is_p("(")){ lex_next();
                                if(T.kind==T_ID){ int q=0; for(;T.s[q]&&q<47;q++) sp->cvar[k][q]=T.s[q]; sp->cvar[k][q]=0; lex_next(); }
                                while(!is_p(")")&&T.kind!=T_EOF) lex_next();
                                if(is_p(")")) lex_next(); }
                            if(is_p(":")) { lex_next(); if(skip_type()) { lex_load(sv); return -1; } }
                            if(is_p("->")) lex_next();
                            if(!is_p("{")) { lex_load(sv); return a2c_fail("select: expected '{' after case"); }
                            sp->cbody[k]=Ts; skip_block(); sp->ncase++;
                        } else if (is_kw("timeout")) {
                            lex_next(); if(T.kind==T_NUM) lex_next();
                            if(is_p("->")) lex_next();
                            if(!is_p("{")) { lex_load(sv); return a2c_fail("select: expected '{' after timeout"); }
                            sp->tbody=Ts; skip_block();
                        } else { lex_load(sv); return a2c_fail("select: expected 'case' or 'timeout'"); }
                    }
                    if(is_p("}")) lex_next();
                    if(sp->ncase==0) { lex_load(sv); return a2c_fail("select: no case"); }
                    sp->rest = Ts;                      /* select の後ろ */
                    NSEL++;
                    /* このクラスに __sel が無ければ足す */
                    if (SELFIELD[ci] < 0) {
                        class_t *c = &CLS[ci];
                        if (c->nfield >= MAXFIELD) { lex_load(sv); return a2c_fail("too many fields (__sel)"); }
                        const char *nm = "__sel";
                        int q=0; for(;nm[q];q++) c->field[c->nfield][q]=nm[q]; c->field[c->nfield][q]=0;
                        c->finit[c->nfield] = 0; c->finit_bool[c->nfield] = 0;
                        SELFIELD[ci] = c->nfield;
                        c->nfield++; if(c->nfield>MAXF) MAXF=c->nfield;
                    }
                    continue;
                }
                lex_next();
            }
        }
    }
    lex_load(sv);
    return 0;
}

/* このメソッドを待っている select（横取り）を探す */
static sel_t *sel_intercept(int ci, int mid, int *which)
{
    for (int i=0;i<NSEL;i++) {
        if (SEL[i].cls != ci) continue;
        for (int k=0;k<SEL[i].ncase;k++)
            if (SEL[i].cmid[k]==mid) { if(which)*which=k; return &SEL[i]; }
    }
    return 0;
}
/* このメソッド自身が持つ select */
static sel_t *sel_owner(int ci, int mi)
{
    for (int i=0;i<NSEL;i++) if (SEL[i].cls==ci && SEL[i].owner==mi) return &SEL[i];
    return 0;
}

/* ---- output ------------------------------------------------------- */
static char *OUT; static int OCAP, OPOS;
static void op_c(char c){ if(OPOS<OCAP-1) OUT[OPOS++]=c; }
static void op_s(const char *s){ while(*s) op_c(*s++); }
static void op_n(long v){ char b[24]; int n=0; if(v<0){op_c('-');v=-v;} if(!v){op_c('0');return;} while(v){b[n++]=(char)('0'+v%10);v/=10;} while(n)op_c(b[--n]); }
static void emit_ident(const char *name)
{
    if (name[0]=='s'&&name[1]=='e'&&name[2]=='l'&&name[3]=='f'&&name[4]==0) { op_s("v_int(self)"); return; }
    if (!is_local(name) && CC>=0 && field_idx(&CLS[CC],name)>=0) {
        op_s("g_obj[self].f["); op_n(field_idx(&CLS[CC],name)); op_s("]"); return;
    }
    op_s("v_"); op_s(name);
}
static void emit_node(int i);
static void emit_args_filled(int *args,int nargs)
{ for(int k=0;k<4;k++){ op_s(", "); if(k<nargs) emit_node(args[k]); else op_s("v_int(0)"); } }
static void emit_node(int i)
{
    enode_t *e;
    /* 無効ノード（解析失敗）と、既に失敗が立っている状態では何も出さない。
       これが無いと 0 番ノードを辿って無限再帰する。 */
    if (i <= 0 || i >= MAXNODE || a2c_err[0]) { op_s("v_int(0)"); return; }
    e=&ND[i];
    switch(e->k){
    case N_NIL: op_s("v_int(0)"); break;
    case N_INT: op_s("v_int("); op_n(e->num); op_c(')'); break;
    /* 真偽値は専用の値にする。印字で true / false に戻すため（正典どおり）。 */
    case N_BOOL: op_s("v_bool("); op_n(e->num); op_c(')'); break;
    case N_STR: op_s("v_str(\""); op_s(e->s); op_s("\")"); break;
    case N_ID:  emit_ident(e->s); break;
    case N_NEW: {
        /* init は new のときに必ず呼ばれる（正典）。以前は引数があるときだけ
           呼んでいたので、`new C()` で `method init()` が黙って走らなかった。
           Pi 3 側（compile.ml）も同じ穴だったので両方直した。 */
        int im = -1;
        for (int k=0; k<CLS[e->ci].nmethod; k++)
            if (a2c_streq(CLS[e->ci].method[k].name,"init")) { im = meth_id("init"); break; }
        if (e->nargs && im < 0) { a2c_fail("new: class has no init to take arguments"); break; }
        if (im >= 0) {
            op_s("v_int(__new_init(g_spawn("); op_n(e->ci); op_s("), "); op_n(im);
            emit_args_filled(e->args, e->nargs); op_s("))");
        } else { op_s("v_int(g_spawn("); op_n(e->ci); op_s("))"); }
        break; }
    case N_PRINT: op_s("v_print("); if(e->nargs) emit_node(e->args[0]); else op_s("v_int(0)"); op_c(')'); break;
    case N_AICALL: op_s("v_ai_call("); if(e->nargs) emit_node(e->args[0]); else op_s("v_str(\"\")"); op_c(')'); break;
    case N_NOW: op_s("dispatch(v_int_of("); emit_node(e->a); op_s("), "); op_n(e->mid); emit_args_filled(e->args,e->nargs); op_c(')'); break;
    case N_WAIT: op_s("cc_wait("); emit_node(e->args[0]); op_c(')'); break;
    case N_WEB:
        if (e->num) {                                  /* web_expose(path, "名前") */
            op_s("cc_web_expose("); emit_node(e->args[0]);
            op_s(", v_int_of(v_"); op_s(ND[e->args[1]].s); op_c(')'); op_c(')');
        } else {                                       /* web_listen(port) */
            op_s("cc_web_listen("); emit_node(e->args[0]); op_c(')');
        }
        break;
    case N_FUT:
        op_s("cc_future(v_int_of("); emit_node(e->a); op_s("), "); op_n(e->mid);
        emit_args_filled(e->args,e->nargs); op_c(')');
        break;
    case N_AWAIT:   op_s("cc_await("); emit_node(e->a); op_c(')'); break;
    case N_AWAITDL:
        op_s("cc_await_dl("); emit_node(e->a); op_s(", "); op_n(e->num);
        op_s(", "); emit_node(e->b); op_c(')');
        break;
    case N_NOWDL:
        op_s("__dl_call(v_int_of("); emit_node(e->a); op_s("), "); op_n(e->mid);
        emit_args_filled(e->args,e->nargs);
        op_s(", "); op_n(e->num); op_s(", "); emit_node(e->b); op_c(')');
        break;
    case N_UN:
        if(e->op==O_NOT){ op_s("v_not("); emit_node(e->a); op_c(')'); }
        else { op_s("v_sub(v_int(0), "); emit_node(e->a); op_c(')'); }
        break;
    case N_BIN: {
        const char *fn=0; int swap=0;
        switch(e->op){
        case O_ADD:fn="v_add";break; case O_SUB:fn="v_sub";break; case O_MUL:fn="v_mul";break; case O_DIV:fn="v_div";break;
        case O_LT:fn="v_lt";break;  case O_LE:fn="v_le";break;  case O_GT:fn="v_lt";swap=1;break; case O_GE:fn="v_le";swap=1;break;
        case O_EQ:fn="v_eq";break;  case O_NE:fn="v_ne";break;  case O_AND:fn="v_and";break; case O_OR:fn="v_or";break; default:fn="v_add";
        }
        op_s(fn); op_c('(');
        if(swap){ emit_node(e->b); op_s(", "); emit_node(e->a); }
        else    { emit_node(e->a); op_s(", "); emit_node(e->b); }
        op_c(')'); break; }
    }
}

/* ---- statement emit (walks tokens) -------------------------------- */
static void emit_stmt(void);
static void emit_block(void)             /* T must be '{' */
{ op_s("{\n"); lex_next(); while(!is_p("}")&&T.kind!=T_EOF&&!a2c_err[0]) emit_stmt(); if(is_p("}")) lex_next(); op_s("}\n"); }

static void emit_send(void)
{
    lex_next();                          /* past 'send' */
    int obj=parse_primary();
    if(!is_p(".")){ a2c_fail("send: expected .method"); return; } lex_next();
    if(T.kind!=T_ID){ a2c_fail("send: expected method"); return; }
    int mid=meth_id(T.s); lex_next();
    int args[4],na=0;
    if(is_p("(")){ lex_next(); while(!is_p(")")&&T.kind!=T_EOF&&!a2c_err[0]){ if(na<4) args[na++]=parse_expr(); else parse_expr(); if(is_p(","))lex_next(); } if(is_p(")"))lex_next(); }
    if(is_p(";")) lex_next();
    op_s("  enqueue(v_int_of("); emit_node(obj); op_s("), "); op_n(mid); emit_args_filled(args,na); op_s(");\n");
    /* トップレベルでは、積んだらその場で配る。Pi 3 はアクターが実プロセスなので
       send した相手が先に走る。Pi 5 は FIFO に積むだけなので、後続の now が
       先に相手のメソッドへ入ってしまい select の横取りが効かない。順序を揃える。 */
    if (CC < 0) op_s("  cc_pump_now();\n");
}
static void emit_stmt(void)
{
    if (T.kind==T_EOF) return;
    if (is_p("{")) { op_s("  "); emit_block(); return; }
    if (is_kw("var")) {
        lex_next(); if(T.kind!=T_ID){a2c_fail("var: expected name");return;} char nm[48]; cpid(nm); lex_next();
        if(is_p(":")) { lex_next(); if(skip_type()) return; }      /* var x: int = ... */
        /* トップレベルの var は大域として先に宣言済みなので、ここでは代入だけ。 */
        if (CC<0 && is_topvar(nm)) op_s("  v_"), op_s(nm), op_s(" = ");
        else { op_s("  int v_"); op_s(nm); op_s(" = "); }
        if(is_p("=")){ lex_next(); int e=parse_expr(); emit_node(e); } else op_s("v_int(0)");
        op_s(";\n"); add_local(nm); if(is_p(";"))lex_next(); return;
    }
    if (is_kw("if")) {
        lex_next(); if(is_p("("))lex_next(); int c=parse_expr(); if(is_p(")"))lex_next();
        op_s("  if (v_truthy("); emit_node(c); op_s(")) "); emit_stmt();
        if(is_kw("else")){ lex_next(); op_s("  else "); emit_stmt(); }
        return;
    }
    if (is_kw("while")) {
        lex_next(); if(is_p("("))lex_next(); int c=parse_expr(); if(is_p(")"))lex_next(); if(is_kw("do"))lex_next();
        op_s("  while (v_truthy("); emit_node(c); op_s(")) "); emit_stmt(); return;
    }
    if (is_kw("return")) {
        lex_next();
        if(is_p(";")){ op_s("  return 0;\n"); lex_next(); }
        else { int e=parse_expr(); op_s("  return "); emit_node(e); op_s(";\n"); if(is_p(";"))lex_next(); }
        return;
    }
    if (is_kw("send")) { emit_send(); return; }
    /* reply(e) — この装置では戻り値そのもの。`now o.m()` は dispatch の
       返り値を受けるので return と同じ。後ろに文が続くと意味がずれるので断る。 */
    if (is_kw("reply")) {
        lex_next(); if(!is_p("(")) { a2c_fail("reply: expected '('"); return; } lex_next();
        op_s("  return ");
        if (is_p(")")) op_s("v_int(0)");
        else { int e=parse_expr(); if(a2c_err[0]) return; emit_node(e); }
        op_s(";\n");
        if(!is_p(")")) { a2c_fail("reply: expected ')'"); return; } lex_next();
        if(is_p(";")) lex_next();
        if(!is_p("}") && T.kind!=T_EOF)
            a2c_fail("reply must be the last statement of its block on this device");
        return;
    }
    /* select は Pi 3 と同じ「フラグを立てて終わる」形に落とす。
       本体（case / timeout / 残り）は scan_selects が控えてあり、
       横取りとして相手のメソッドの先頭に出す。ここでは番号を立てるだけ。 */
    if (is_kw("select")) {
        sel_t *sp = (CC>=0) ? sel_owner(CC, CURMETH) : 0;
        if (!sp) { a2c_fail("select: メソッドの中でだけ使えます"); return; }
        /* 走査時と同じところまで読み飛ばす */
        lex_next();
        if(is_p("{")) { L = sp->rest; lex_next(); }
        op_s("  g_obj[self].f["); op_n(SELFIELD[CC]); op_s("] = v_int("); op_n(sp->sid); op_s(");\n");
        return;
    }
    /* assignment (ID '=' expr) or expression statement */
    if (T.kind==T_ID) {
        lsave_t sv=lex_save(); char nm[48]; cpid(nm); lex_next();
        if (is_p("=")) { lex_next(); int e=parse_expr(); op_s("  "); emit_ident(nm); op_s(" = "); emit_node(e); op_s(";\n"); if(is_p(";"))lex_next(); return; }
        lex_load(sv);
    }
    { int e=parse_expr(); op_s("  "); emit_node(e); op_s(";\n"); if(is_p(";"))lex_next(); }
}

/* ---- whole-program emit ------------------------------------------- */
static void emit_program(void)
{
    OPOS=0;
    op_s("/* AIPL -> C  (on-device abcl2c: self-contained integer subset) */\n");
    op_s("struct Obj { int cls; int f["); op_n(MAXF); op_s("]; };\n");
    op_s("struct Obj g_obj[2048];\nint g_nobj;\n\n");

    /* 機内 cc はプロトタイプ宣言を持たない（前方参照の呼び出しは通る）。
       ここで dispatch を宣言してはいけない。 */
    op_s("int __new_init(int id, int meth, int a0, int a1, int a2, int a3) {\n"
         "  if (id >= 0) { dispatch(id, meth, a0, a1, a2, a3); }\n  return id;\n}\n");
    /* 期限つき呼び出しの受け皿。cc_now_ms と dispatch だけで書ける。
       機内 cc は仮引数 8 個までなので self は渡さない（cc_call も捨てている）。 */
    op_s("int __dl_call(int to, int meth, int a0, int a1, int a2, int a3, int ms, int elsev) {\n"
         "  int t0; int r;\n"
         "  t0 = v_int_of(cc_now_ms());\n"
         "  r = dispatch(to, meth, a0, a1, a2, a3);\n"
         "  if (v_int_of(cc_now_ms()) - t0 > ms) { return elsev; }\n"
         "  return r;\n}\n");
    /* アクタを作れなかったら黙って -1 を返さず、はっきり言う。
       黙ると「なぜか動かない」になり、実機では領域外参照まで進む。 */
    op_s("int g_spawn(int cls) {\n  int id; id = cc_actor_new();\n"
         "  if (id < 0) { v_print(v_str(\"cc: アクタを作れませんでした（資源不足）\")); return -1; }\n");
    op_s("  g_nobj = g_nobj + 1;\n  g_obj[id].cls = cls;\n");
    for (int ci=0; ci<NCLS; ci++) {
        op_s("  if (cls == "); op_n(ci); op_s(") {\n");
        for (int f=0; f<CLS[ci].nfield; f++){ op_s("    g_obj[id].f["); op_n(f);
            op_s(CLS[ci].finit_bool[f] ? "] = v_bool(" : "] = v_int(");
            op_n(CLS[ci].finit[f]); op_s(");\n"); }
        op_s("  }\n");
    }
    op_s("  return id;\n}\n\n");

    scan_selects();
    scan_topvars();
    for (int i=0;i<NTOPV;i++){ op_s("int v_"); op_s(TOPV[i]); op_s(";\n"); }
    if (NTOPV) op_c('\n');

    for (int ci=0; ci<NCLS; ci++) {
        for (int mi=0; mi<CLS[ci].nmethod; mi++) {
            op_s("int m_"); op_s(CLS[ci].name); op_c('_'); op_s(CLS[ci].method[mi].name);
            op_s("(int self, int a0, int a1, int a2, int a3) {\n");
            CC=ci; NLOCAL=0; CURMETH=mi;
            for (int p=0; p<CLS[ci].method[mi].nparam; p++){ op_s("  int v_"); op_s(CLS[ci].method[mi].param[p]); op_s(" = a"); op_n(p); op_s(";\n"); add_local(CLS[ci].method[mi].param[p]); }
            /* このメソッドを待っている select があれば、先頭で横取りする。
               case の仮引数は受け側の仮引数へ位置で対応づく（Pi 3 と同じ）。 */
            int which = -1;
            sel_t *ip = sel_intercept(ci, meth_id(CLS[ci].method[mi].name), &which);
            if (ip) {
                op_s("  if (v_int_of(g_obj[self].f["); op_n(SELFIELD[ci]); op_s("]) == "); op_n(ip->sid); op_s(") {\n");
                op_s("    g_obj[self].f["); op_n(SELFIELD[ci]); op_s("] = v_int(0);\n");
                if (ip->cvar[which][0] && CLS[ci].method[mi].nparam>0
                    && !a2c_streq(ip->cvar[which], CLS[ci].method[mi].param[0])) {
                    op_s("    int v_"); op_s(ip->cvar[which]); op_s(" = a0;\n");
                    add_local(ip->cvar[which]);
                }
                L=ip->cbody[which]; lex_next(); emit_block();      /* case 本体 */
                if (ip->rest) {                                     /* select の残り */
                    L=ip->rest; lex_next();
                    while (!is_p("}") && T.kind!=T_EOF && !a2c_err[0]) emit_stmt();
                }
                op_s("    return v_int(0);\n  }\n");
                NLOCAL=0;
                for (int p=0; p<CLS[ci].method[mi].nparam; p++) add_local(CLS[ci].method[mi].param[p]);
            }
            L=CLS[ci].method[mi].body; lex_next();   /* re-lex the body from '{' */
            emit_block();
            op_s("  return 0;\n}\n\n");
        }
    }

    /* select の timeout 節。Pi 3 は別アクタ __Timer に wait(ms) させて
       「まだ待っていたら timeout 節を走らせる」形にしている。この装置には
       待てる実行体が無いので、FIFO を配り切っても誰も名乗り出なかったときに
       走らせる ―― 「そのメッセージは来なかった」という意味では同じ。
       ★ 期限の値（ミリ秒）は使えない。時間ではなく「もう来ない」で判定する。 */
    int nsel_tmo = 0;
    for (int i=0;i<NSEL;i++) if (SEL[i].tbody) nsel_tmo++;
    if (nsel_tmo) {
        for (int i=0;i<NSEL;i++) {
            if (!SEL[i].tbody) continue;
            sel_t *sp = &SEL[i];
            op_s("int __seltmo"); op_n(sp->sid); op_s("(int self) {\n");
            CC=sp->cls; NLOCAL=0; CURMETH=sp->owner;
            L=sp->tbody; lex_next(); emit_block();          /* timeout 本体 */
            if (sp->rest) {                                  /* select の残り */
                L=sp->rest; lex_next();
                while (!is_p("}") && T.kind!=T_EOF && !a2c_err[0]) emit_stmt();
            }
            op_s("  return v_int(0);\n}\n\n");
        }
        op_s("int __sel_sweep() {\n  int i;\n  i = 0;\n  while (i < 1024) {\n");
        for (int i=0;i<NSEL;i++) {
            if (!SEL[i].tbody) continue;
            sel_t *sp = &SEL[i];
            op_s("    if (g_obj[i].cls == "); op_n(sp->cls); op_s(") {\n");
            op_s("      if (v_int_of(g_obj[i].f["); op_n(SELFIELD[sp->cls]); op_s("]) == "); op_n(sp->sid); op_s(") {\n");
            op_s("        g_obj[i].f["); op_n(SELFIELD[sp->cls]); op_s("] = v_int(0);\n");
            op_s("        __seltmo"); op_n(sp->sid); op_s("(i);\n      }\n    }\n");
        }
        op_s("    i = i + 1;\n  }\n  return 0;\n}\n\n");
    }

    /* self が負なら何もしない。アクタを作れなかったとき（NPROC 枯渇など）
       g_spawn は -1 を返すので、そのまま dispatch に渡ると g_obj[-1] を読む
       ―― 領域外参照で板が壊れる。Pi 4 の actorproc.c 自身が「それで箱が
       壊れる」と書いている。三機とも同じ形なので、生成側で塞ぐ。 */
    op_s("int dispatch(int self, int meth, int a0, int a1, int a2, int a3) {\n"
         "  int c;\n  if (self < 0) { return v_int(0); }\n  c = g_obj[self].cls;\n");
    for (int ci=0; ci<NCLS; ci++) {
        op_s("  if (c == "); op_n(ci); op_s(") {\n");
        for (int mi=0; mi<CLS[ci].nmethod; mi++){ op_s("    if (meth == "); op_n(meth_id(CLS[ci].method[mi].name)); op_s(") return m_"); op_s(CLS[ci].name); op_c('_'); op_s(CLS[ci].method[mi].name); op_s("(self, a0, a1, a2, a3);\n"); }
        op_s("  }\n");
    }
    op_s("  return 0;\n}\n\n");

    op_s("int __method_id(int name) {\n");
    for (int i=0;i<NMNAME;i++){ op_s("  if (v_truthy(v_eq(name, v_str(\""); op_s(MNAME[i]); op_s("\")))) return v_int("); op_n(i); op_s(");\n"); }
    op_s("  return v_int(-1);\n}\n\n");

    op_s("int __nobj() { return v_int(g_nobj); }\n");
    op_s("int __cls_name(int cls) {\n");
    for (int ci=0; ci<NCLS; ci++){ op_s("  if (cls == "); op_n(ci); op_s(") return v_str(\""); op_s(CLS[ci].name); op_s("\");\n"); }
    op_s("  return v_str(\"?\");\n}\n");
    op_s("int __obj_cls(int id) { return v_int(g_obj[id].cls); }\n");
    op_s("int __obj_field(int id, int fidx) { return g_obj[id].f[fidx]; }\n\n");

    op_s("int main() {\n");
    CC=-1; NLOCAL=0;
    L=TOPLEVEL; lex_next();
    while (T.kind!=T_EOF && !a2c_err[0]) emit_stmt();
    if (nsel_tmo) op_s("  cc_pump_now();\n  __sel_sweep();\n");
    op_s("  return 0;\n}\n");
    if (OPOS<OCAP) OUT[OPOS]=0;
}

/* ---- public API --------------------------------------------------- */
int abcl2c(const char *src, int srclen, char *out, int outcap)
{
    (void)srclen;
    /* ND[0] は「解析に失敗した」を表す無効ノード。parse_* は失敗時に 0 を
       返すので、0 を実在のノードにしてはいけない。 */
    NND=1; ND[0].k=N_NIL; ND[0].nargs=0; a2c_err[0]=0;
    if (parse_structure(src) < 0) return -1;
    OUT=out; OCAP=outcap; OPOS=0;
    emit_program();
    if (a2c_err[0]) return -1;
    if (OPOS >= OCAP-1) return a2c_fail("translated C exceeds buffer");
    return OPOS;
}

#ifdef ABCL2C_HOST_TEST
int main(int argc, char **argv)
{
    if (argc<2){ fprintf(stderr,"usage: %s file.abcl\n",argv[0]); return 2; }
    FILE *f=fopen(argv[1],"rb"); if(!f){ perror("open"); return 2; }
    static char src[65536]; int n=(int)fread(src,1,sizeof src-1,f); src[n]=0; fclose(f);
    static char out[262144];
    int r=abcl2c(src,n,out,sizeof out);
    if(r<0){ fprintf(stderr,"abcl2c: %s\n",abcl2c_error()); return 1; }
    fwrite(out,1,r,stdout);
    return 0;
}
#endif
