// cc/abcl2c.c — on-device AIPL (.abcl) -> C translator (the xinu-jit subset).
//
// Turns a real AIPL actor program (classes / fields / methods / new / send /
// now / if / while / return / print / arithmetic / strings) into the
// self-contained integer-subset C the on-device compiler (cc) JITs.  Same
// shape as the host `aipl2c --xinu-jit`, but on the device so `cc foo.abcl`
// and `make foo.abcl` run real AIPL with no host round-trip.
//
// Runtime seam (provided by cc.c via cc_resolve_extern):
//   v_int v_str v_add v_sub v_mul v_div v_lt v_le v_eq v_ne v_and v_or v_not
//   v_truthy v_int_of v_print enqueue cc_actor_new   (dispatch is generated)
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
typedef struct { int kind; char s[64]; long num; } tok_t;

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
    if (al(*L)) { int n=0; while (an(*L)&&n<63) T.s[n++]=*L++; T.s[n]=0; T.kind=T_ID; return; }
    if (dg(*L)) { long v=0; while (dg(*L)) v=v*10+(*L++-'0'); if (*L=='.'){L++;while(dg(*L))L++;} T.kind=T_NUM; T.num=v; return; }
    if (*L=='"') { L++; int n=0; while (*L && *L!='"' && n<62){ if(*L=='\\'&&L[1]) T.s[n++]=*L++; T.s[n++]=*L++; } if(*L=='"')L++; T.s[n]=0; T.kind=T_STR; return; }
    { char a=L[0],b=L[1]; const char *two=0;
      if(a=='='&&b=='=')two="==";else if(a=='!'&&b=='=')two="!=";else if(a=='<'&&b=='=')two="<=";
      else if(a=='>'&&b=='=')two=">=";else if(a=='&'&&b=='&')two="&&";else if(a=='|'&&b=='|')two="||";
      if(two){T.s[0]=two[0];T.s[1]=two[1];T.s[2]=0;L+=2;T.kind=T_PUNCT;return;}
      T.s[0]=a;T.s[1]=0;L++;T.kind=T_PUNCT;return; }
}

typedef struct { const char *L; const char *Ts; tok_t T; } lsave_t;
static lsave_t lex_save(void) { lsave_t s; s.L=L; s.Ts=Ts; s.T=T; return s; }
static void    lex_load(lsave_t s) { L=s.L; Ts=s.Ts; T=s.T; }

static int is_p(const char *p) { return T.kind==T_PUNCT && T.s[0]==p[0] && (p[1]==0 ? T.s[1]==0 : T.s[1]==p[1]); }
static int is_kw(const char *k) { if (T.kind!=T_ID) return 0; int i=0; for(;k[i];i++) if(T.s[i]!=k[i]) return 0; return T.s[i]==0; }
static void cpid(char *d) { int i=0; for(;T.s[i]&&i<47;i++) d[i]=T.s[i]; d[i]=0; }
static void cpid_to(char *d){ int i=0; for(;T.s[i]&&i<63;i++) d[i]=T.s[i]; d[i]=0; }

/* ---- program model ------------------------------------------------ */
#define MAXCLASS 16
#define MAXFIELD 16
#define MAXMETHOD 24
#define MAXPARAM 4
#define MAXMNAME 48
typedef struct {
    char name[48];
    char field[MAXFIELD][48]; long finit[MAXFIELD]; int nfield;
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
        while(!is_p("}") && T.kind!=T_EOF) {
            if (is_kw("var")) {
                lex_next(); if(T.kind!=T_ID) return a2c_fail("field: expected name");
                if(c->nfield>=MAXFIELD) return a2c_fail("too many fields");
                cpid(c->field[c->nfield]); long init=0; lex_next();
                if(is_p("=")){ lex_next(); if(T.kind==T_NUM) init=T.num; while(!is_p(";")&&T.kind!=T_EOF) lex_next(); }
                c->finit[c->nfield]=init; c->nfield++; if(c->nfield>MAXF) MAXF=c->nfield;
                if(is_p(";")) lex_next();
            } else if (is_kw("method")) {
                lex_next(); if(T.kind!=T_ID) return a2c_fail("method: expected name");
                if(c->nmethod>=MAXMETHOD) return a2c_fail("too many methods");
                int mi=c->nmethod++; cpid(c->method[mi].name); meth_id(c->method[mi].name); c->method[mi].nparam=0;
                lex_next(); if(!is_p("(")) return a2c_fail("method: expected '('"); lex_next();
                while(!is_p(")")&&T.kind!=T_EOF){ if(T.kind==T_ID && c->method[mi].nparam<MAXPARAM) cpid(c->method[mi].param[c->method[mi].nparam++]); lex_next(); if(is_p(",")) lex_next(); }
                lex_next(); if(!is_p("{")) return a2c_fail("method: expected '{'");
                c->method[mi].body=Ts;       /* points at the '{' */
                skip_block();
            } else return a2c_fail("class body: expected var/method");
        }
        if(is_p("}")) lex_next();
    }
    return 0;
}

/* ---- expression AST ----------------------------------------------- */
enum { N_INT, N_STR, N_ID, N_NEW, N_NOW, N_PRINT, N_BIN, N_UN };
enum { O_ADD,O_SUB,O_MUL,O_DIV,O_LT,O_LE,O_GT,O_GE,O_EQ,O_NE,O_AND,O_OR,O_NOT,O_NEG };
typedef struct { int k; long num; char s[64]; int op; int a,b; int ci,mid; int args[4]; int nargs; } enode_t;
#define MAXNODE 1024
static enode_t ND[MAXNODE]; static int NND;
static int mknode(int k){ if(NND>=MAXNODE) return 0; int i=NND++; ND[i].k=k; ND[i].nargs=0; return i; }

/* current emit context */
static int   CC;                         /* current class idx, -1 = top level  */
static char  LOCAL[64][48]; static int NLOCAL;
static int   is_local(const char *n){ for(int i=0;i<NLOCAL;i++){int k=0;for(;n[k];k++)if(LOCAL[i][k]!=n[k])goto no; if(LOCAL[i][k]==0)return 1; no:;} return 0; }
static void  add_local(const char *n){ if(NLOCAL<64){int k=0;for(;n[k]&&k<47;k++)LOCAL[NLOCAL][k]=n[k];LOCAL[NLOCAL][k]=0;NLOCAL++;} }

static int parse_expr(void);
static int parse_primary(void)
{
    if (T.kind==T_NUM) { int n=mknode(N_INT); ND[n].num=T.num; lex_next(); return n; }
    if (T.kind==T_STR) { int n=mknode(N_STR); cpid_to(ND[n].s); lex_next(); return n; }
    if (is_kw("new")) { lex_next(); if(T.kind!=T_ID){a2c_fail("new: expected class");return 0;}
        int ci=class_idx(T.s); int n=mknode(N_NEW); ND[n].ci=ci; lex_next();
        if(is_p("(")){lex_next(); if(is_p(")"))lex_next();} return n; }
    if (is_kw("now")) { lex_next(); int obj=parse_primary();
        if(!is_p(".")){a2c_fail("now: expected .method");return 0;} lex_next();
        if(T.kind!=T_ID){a2c_fail("now: expected method");return 0;}
        int n=mknode(N_NOW); ND[n].mid=meth_id(T.s); ND[n].a=obj; lex_next();
        if(is_p("(")){lex_next(); while(!is_p(")")&&T.kind!=T_EOF){ if(ND[n].nargs<4) ND[n].args[ND[n].nargs++]=parse_expr(); else parse_expr(); if(is_p(","))lex_next(); } if(is_p(")"))lex_next();}
        return n; }
    if (is_kw("self")) { int n=mknode(N_ID); ND[n].s[0]='s';ND[n].s[1]='e';ND[n].s[2]='l';ND[n].s[3]='f';ND[n].s[4]=0; lex_next(); return n; }
    if (is_p("(")) { lex_next(); int e=parse_expr(); if(is_p(")"))lex_next(); return e; }
    if (T.kind==T_ID) {
        char nm[48]; cpid(nm); lex_next();
        if (is_p("(")) {                 /* call: print/puts only */
            int n=mknode(N_PRINT); lex_next();
            while(!is_p(")")&&T.kind!=T_EOF){ if(ND[n].nargs<4) ND[n].args[ND[n].nargs++]=parse_expr(); else parse_expr(); if(is_p(","))lex_next(); }
            if(is_p(")"))lex_next();
            /* only print/puts supported; both -> v_print(arg0) */
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
static int parse_rel(void){ int a=parse_add(); for(;;){ int op; if(is_p("<="))op=O_LE; else if(is_p(">="))op=O_GE; else if(is_p("<"))op=O_LT; else if(is_p(">"))op=O_GT; else break; lex_next(); a=bin(op,a,parse_add()); } return a; }
static int parse_eql(void){ int a=parse_rel(); for(;;){ int op; if(is_p("=="))op=O_EQ; else if(is_p("!="))op=O_NE; else break; lex_next(); a=bin(op,a,parse_rel()); } return a; }
static int parse_and(void){ int a=parse_eql(); while(is_p("&&")){ lex_next(); a=bin(O_AND,a,parse_eql()); } return a; }
static int parse_or (void){ int a=parse_and(); while(is_p("||")){ lex_next(); a=bin(O_OR,a,parse_and()); } return a; }
static int parse_expr(void){ return parse_or(); }

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
    enode_t *e=&ND[i];
    switch(e->k){
    case N_INT: op_s("v_int("); op_n(e->num); op_c(')'); break;
    case N_STR: op_s("v_str(\""); op_s(e->s); op_s("\")"); break;
    case N_ID:  emit_ident(e->s); break;
    case N_NEW: op_s("v_int(g_spawn("); op_n(e->ci); op_s("))"); break;
    case N_PRINT: op_s("v_print("); if(e->nargs) emit_node(e->args[0]); else op_s("v_int(0)"); op_c(')'); break;
    case N_NOW: op_s("dispatch(v_int_of("); emit_node(e->a); op_s("), "); op_n(e->mid); emit_args_filled(e->args,e->nargs); op_c(')'); break;
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
{ op_s("{\n"); lex_next(); while(!is_p("}")&&T.kind!=T_EOF) emit_stmt(); if(is_p("}")) lex_next(); op_s("}\n"); }

static void emit_send(void)
{
    lex_next();                          /* past 'send' */
    int obj=parse_primary();
    if(!is_p(".")){ a2c_fail("send: expected .method"); return; } lex_next();
    if(T.kind!=T_ID){ a2c_fail("send: expected method"); return; }
    int mid=meth_id(T.s); lex_next();
    int args[4],na=0;
    if(is_p("(")){ lex_next(); while(!is_p(")")&&T.kind!=T_EOF){ if(na<4) args[na++]=parse_expr(); else parse_expr(); if(is_p(","))lex_next(); } if(is_p(")"))lex_next(); }
    if(is_p(";")) lex_next();
    op_s("  enqueue(v_int_of("); emit_node(obj); op_s("), "); op_n(mid); emit_args_filled(args,na); op_s(");\n");
}
static void emit_stmt(void)
{
    if (T.kind==T_EOF) return;
    if (is_p("{")) { op_s("  "); emit_block(); return; }
    if (is_kw("var")) {
        lex_next(); if(T.kind!=T_ID){a2c_fail("var: expected name");return;} char nm[48]; cpid(nm); lex_next();
        op_s("  int v_"); op_s(nm); op_s(" = ");
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

    op_s("int g_spawn(int cls) {\n  int id; id = cc_actor_new();\n  if (id < 0) { return -1; }\n");
    op_s("  g_nobj = g_nobj + 1;\n  g_obj[id].cls = cls;\n");
    for (int ci=0; ci<NCLS; ci++) {
        op_s("  if (cls == "); op_n(ci); op_s(") {\n");
        for (int f=0; f<CLS[ci].nfield; f++){ op_s("    g_obj[id].f["); op_n(f); op_s("] = v_int("); op_n(CLS[ci].finit[f]); op_s(");\n"); }
        op_s("  }\n");
    }
    op_s("  return id;\n}\n\n");

    for (int ci=0; ci<NCLS; ci++) {
        for (int mi=0; mi<CLS[ci].nmethod; mi++) {
            op_s("int m_"); op_s(CLS[ci].name); op_c('_'); op_s(CLS[ci].method[mi].name);
            op_s("(int self, int a0, int a1, int a2, int a3) {\n");
            CC=ci; NLOCAL=0;
            for (int p=0; p<CLS[ci].method[mi].nparam; p++){ op_s("  int v_"); op_s(CLS[ci].method[mi].param[p]); op_s(" = a"); op_n(p); op_s(";\n"); add_local(CLS[ci].method[mi].param[p]); }
            L=CLS[ci].method[mi].body; lex_next();   /* re-lex the body from '{' */
            emit_block();
            op_s("  return 0;\n}\n\n");
        }
    }

    op_s("int dispatch(int self, int meth, int a0, int a1, int a2, int a3) {\n  int c; c = g_obj[self].cls;\n");
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
    while (T.kind!=T_EOF) emit_stmt();
    op_s("  return 0;\n}\n");
    if (OPOS<OCAP) OUT[OPOS]=0;
}

/* ---- public API --------------------------------------------------- */
int abcl2c(const char *src, int srclen, char *out, int outcap)
{
    (void)srclen;
    NND=0; a2c_err[0]=0;
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
