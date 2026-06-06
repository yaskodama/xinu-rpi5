// cc/parse.c — lexer + recursive-descent parser for the on-device C
// subset.  Produces the function/AST list consumed by codegen.c.

#include "ccpriv.h"

/* ---------- small string helpers (no libc) ---------- */

static int ceq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static char *dup_n(const char *s, int n)
{
    char *d = (char *)cc_alloc((unsigned long)n + 1);
    for (int i = 0; i < n; i++) d[i] = s[i];
    d[n] = 0;
    return d;
}

static int is_space(char c)  { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int is_digit(char c)  { return c >= '0' && c <= '9'; }
static int is_alpha(char c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_alnum(char c)  { return is_alpha(c) || is_digit(c); }

/* ====================================================================
 *  Lexer
 * ==================================================================== */

static char read_escape(const char **pp)
{
    const char *p = *pp;            /* p points just past the backslash */
    char c = *p++;
    char r;
    switch (c) {
        case 'n': r = '\n'; break;
        case 't': r = '\t'; break;
        case 'r': r = '\r'; break;
        case '0': r = '\0'; break;
        case '\\': r = '\\'; break;
        case '\'': r = '\''; break;
        case '"': r = '"'; break;
        default:  r = c;    break;
    }
    *pp = p;
    return r;
}

static token_t *new_tok(tokkind_t kind)
{
    token_t *t = (token_t *)cc_alloc(sizeof(token_t));
    t->kind = kind; t->val = 0; t->str = 0; t->slen = 0; t->text = 0; t->next = 0;
    return t;
}

static const char *kw_tab[] = {
    "int", "char", "if", "else", "while", "for", "return", "sizeof",
    "struct", "typedef", "switch", "case", "default", "break", "continue", 0
};

/* exact match of keyword `k` against the n-char span `s` */
static int is_keyword(const char *s, int n)
{
    for (int i = 0; kw_tab[i]; i++) {
        const char *k = kw_tab[i];
        int j = 0;
        while (j < n && k[j] && k[j] == s[j]) j++;
        if (j == n && k[j] == 0) return 1;
    }
    return 0;
}

/* membership test for the single-char punctuator set (defined below) */
static int ceq_chr(const char *set, char c);

token_t *cc_lex(const char *src)
{
    token_t head; head.next = 0;
    token_t *cur = &head;
    const char *p = src;

    while (*p) {
        if (is_space(*p)) { p++; continue; }

        /* line comment */
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        /* block comment */
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            continue;
        }

        if (is_digit(*p)) {
            /* unsigned accumulation so 64-bit constants (e.g. IEEE-754
             * float bit patterns from --xinu-jit) wrap to the right
             * two's-complement value instead of signed-overflow UB. */
            unsigned long v = 0;
            while (is_digit(*p)) { v = v * 10UL + (unsigned long)(*p - '0'); p++; }
            token_t *t = new_tok(TK_NUM); t->val = (long)v;
            cur = cur->next = t;
            continue;
        }

        if (*p == '\'') {                /* char literal */
            p++;
            char c;
            if (*p == '\\') { p++; c = read_escape(&p); }
            else c = *p++;
            if (*p != '\'') { cc_error("unterminated char literal"); return 0; }
            p++;
            token_t *t = new_tok(TK_NUM); t->val = (long)(unsigned char)c;
            cur = cur->next = t;
            continue;
        }

        if (*p == '"') {                 /* string literal */
            p++;
            char buf[1024];
            int n = 0;
            while (*p && *p != '"' && n < (int)sizeof(buf) - 1) {
                if (*p == '\\') { p++; buf[n++] = read_escape(&p); }
                else buf[n++] = *p++;
            }
            if (*p != '"') { cc_error("unterminated string literal"); return 0; }
            p++;
            token_t *t = new_tok(TK_STR);
            t->str = dup_n(buf, n);
            t->slen = n;
            cur = cur->next = t;
            continue;
        }

        if (is_alpha(*p)) {
            const char *s = p;
            while (is_alnum(*p)) p++;
            int n = (int)(p - s);
            token_t *t = new_tok(is_keyword(s, n) ? TK_KW : TK_IDENT);
            t->text = dup_n(s, n);
            cur = cur->next = t;
            continue;
        }

        /* punctuators: try 2-char operators first */
        static const char *two[] = { "==", "!=", "<=", ">=", "&&", "||", "->", 0 };
        int matched = 0;
        for (int i = 0; two[i]; i++) {
            if (p[0] == two[i][0] && p[1] == two[i][1]) {
                token_t *t = new_tok(TK_PUNCT); t->text = dup_n(p, 2);
                cur = cur->next = t; p += 2; matched = 1; break;
            }
        }
        if (matched) continue;

        if (*p && ceq_chr("+-*/%=<>()[]{}&!,;.:", *p)) {
            token_t *t = new_tok(TK_PUNCT); t->text = dup_n(p, 1);
            cur = cur->next = t; p++;
            continue;
        }

        cc_errorc("unexpected character", *p);
        return 0;
    }

    cur->next = new_tok(TK_EOF);
    return head.next;
}

/* membership test for the single-char punctuator set */
static int ceq_chr(const char *set, char c)
{
    for (const char *s = set; *s; s++) if (*s == c) return 1;
    return 0;
}

/* ====================================================================
 *  Types
 * ==================================================================== */

static type_t g_int  = { TY_INT,  8, 0, 0, 0 };
static type_t g_char = { TY_CHAR, 1, 0, 0, 0 };

type_t *ty_int(void)  { return &g_int;  }
type_t *ty_char(void) { return &g_char; }

type_t *ty_ptr_to(type_t *base)
{
    type_t *t = (type_t *)cc_alloc(sizeof(type_t));
    t->kind = TY_PTR; t->size = 8; t->base = base; t->arraylen = 0; t->members = 0;
    return t;
}

type_t *ty_array_of(type_t *base, int len)
{
    type_t *t = (type_t *)cc_alloc(sizeof(type_t));
    t->kind = TY_ARRAY; t->size = base->size * len; t->base = base; t->arraylen = len; t->members = 0;
    return t;
}

int ty_is_ptrlike(type_t *t) { return t && (t->kind == TY_PTR || t->kind == TY_ARRAY); }

static int p_align_up(int n, int a) { return (n + a - 1) / a * a; }

/* ---- named struct registry ---- */
static struct sdef { char *name; type_t *ty; struct sdef *next; } *g_structs;

static type_t *find_struct(const char *name)
{
    for (struct sdef *s = g_structs; s; s = s->next) if (ceq(s->name, name)) return s->ty;
    return 0;
}
static void register_struct(char *name, type_t *ty)
{
    struct sdef *s = (struct sdef *)cc_alloc(sizeof *s);
    s->name = name; s->ty = ty; s->next = g_structs; g_structs = s;
}
static member_t *find_member(type_t *st, const char *name)
{
    if (!st || st->kind != TY_STRUCT) return 0;
    for (member_t *m = st->members; m; m = m->next) if (ceq(m->name, name)) return m;
    return 0;
}

/* ---- typedef registry ---- */
static struct tdef { char *name; type_t *ty; struct tdef *next; } *g_typedefs;

static type_t *find_typedef(const char *name)
{
    for (struct tdef *t = g_typedefs; t; t = t->next) if (ceq(t->name, name)) return t->ty;
    return 0;
}
static void register_typedef(char *name, type_t *ty)
{
    struct tdef *t = (struct tdef *)cc_alloc(sizeof *t);
    t->name = name; t->ty = ty; t->next = g_typedefs; g_typedefs = t;
}

/* ====================================================================
 *  Parser
 * ==================================================================== */

static token_t *tk;          /* current token cursor                     */
static var_t   *cur_locals;  /* locals of the function being parsed       */
static var_t   *g_globals;   /* global variables (whole program)         */
static node_t  *cur_switch;  /* innermost switch being parsed             */

static int is_punct(token_t *t, const char *s) { return t && t->kind == TK_PUNCT && ceq(t->text, s); }
static int is_kw(token_t *t, const char *s)    { return t && t->kind == TK_KW    && ceq(t->text, s); }

static void expect(const char *s)
{
    if (!is_punct(tk, s) && !is_kw(tk, s)) { cc_error("expected token"); return; }
    tk = tk->next;
}

static int consume(const char *s)
{
    if (is_punct(tk, s) || is_kw(tk, s)) { tk = tk->next; return 1; }
    return 0;
}

/* node constructors */
static node_t *new_node(nodekind_t k)
{
    node_t *n = (node_t *)cc_alloc(sizeof(node_t));
    n->kind = k; n->ty = 0;
    n->lhs = n->rhs = n->cond = n->then = n->els = n->init = n->inc = n->body = n->next = n->args = 0;
    n->val = 0; n->var = 0; n->str = 0; n->slen = 0; n->funcname = 0;
    return n;
}

static node_t *new_num(long v)
{
    node_t *n = new_node(ND_NUM); n->val = v; n->ty = ty_int();
    return n;
}

static var_t *find_var(const char *name)
{
    for (var_t *v = cur_locals; v; v = v->next) if (ceq(v->name, name)) return v;
    for (var_t *v = g_globals; v; v = v->next) if (ceq(v->name, name)) return v;
    return 0;
}

static var_t *new_local(const char *name, type_t *ty)
{
    var_t *v = (var_t *)cc_alloc(sizeof(var_t));
    v->name = (char *)name; v->ty = ty; v->offset = 0; v->is_global = 0; v->gaddr = 0;
    v->next = cur_locals; cur_locals = v;
    return v;
}

static var_t *new_global(const char *name, type_t *ty)
{
    var_t *v = (var_t *)cc_alloc(sizeof(var_t));
    unsigned long sz = (ty->size < 8) ? 8 : (unsigned long)ty->size;
    v->name = (char *)name; v->ty = ty; v->offset = 0; v->is_global = 1;
    v->gaddr = (unsigned long)cc_alloc(sz);   /* zeroed arena storage */
    v->next = g_globals; g_globals = v;
    return v;
}

/* forward decls */
static node_t *expr(void);
static node_t *assign(void);
static node_t *stmt(void);

static node_t *new_binary(nodekind_t k, node_t *l, node_t *r)
{
    node_t *n = new_node(k); n->lhs = l; n->rhs = r; n->ty = ty_int();
    return n;
}

/* a + b  with pointer scaling */
static node_t *new_add(node_t *l, node_t *r)
{
    int lp = ty_is_ptrlike(l->ty), rp = ty_is_ptrlike(r->ty);
    if (!lp && !rp) return new_binary(ND_ADD, l, r);
    if (lp && rp) { cc_error("pointer + pointer"); return l; }
    if (!lp && rp) { node_t *t = l; l = r; r = t; }   /* canonicalise: ptr on left */
    int sz = l->ty->base ? l->ty->base->size : 1;
    node_t *scaled = new_binary(ND_MUL, r, new_num(sz));
    node_t *n = new_binary(ND_ADD, l, scaled);
    n->ty = ty_ptr_to(l->ty->base);
    return n;
}

/* a - b  with pointer scaling */
static node_t *new_sub(node_t *l, node_t *r)
{
    int lp = ty_is_ptrlike(l->ty), rp = ty_is_ptrlike(r->ty);
    if (!lp && !rp) return new_binary(ND_SUB, l, r);
    if (lp && !rp) {                                  /* ptr - int */
        int sz = l->ty->base ? l->ty->base->size : 1;
        node_t *scaled = new_binary(ND_MUL, r, new_num(sz));
        node_t *n = new_binary(ND_SUB, l, scaled);
        n->ty = ty_ptr_to(l->ty->base);
        return n;
    }
    if (lp && rp) {                                   /* ptr - ptr = count */
        int sz = l->ty->base ? l->ty->base->size : 1;
        node_t *d = new_binary(ND_SUB, l, r);
        node_t *n = new_binary(ND_DIV, d, new_num(sz));
        n->ty = ty_int();
        return n;
    }
    cc_error("int - pointer");
    return l;
}

static type_t *struct_decl(void);   /* forward (mutually recursive) */

/* parse a base type keyword + pointer stars:
 *   ("int" | "char" | "struct" tag? "{...}"?) "*"* */
static type_t *declspec(void)
{
    type_t *ty;
    if (consume("int"))         ty = ty_int();
    else if (consume("char"))   ty = ty_char();
    else if (consume("struct")) ty = struct_decl();
    else if (tk->kind == TK_IDENT && find_typedef(tk->text)) { ty = find_typedef(tk->text); tk = tk->next; }
    else { cc_error("expected type"); return ty_int(); }
    while (is_punct(tk, "*")) { tk = tk->next; ty = ty_ptr_to(ty); }
    return ty;
}

/* "struct" already consumed.  Parse  tag? ("{" member* "}")? */
static type_t *struct_decl(void)
{
    char *tag = 0;
    if (tk->kind == TK_IDENT) { tag = tk->text; tk = tk->next; }

    if (!is_punct(tk, "{")) {                       /* reference to a named struct */
        if (!tag) { cc_error("anonymous struct without a body"); return ty_int(); }
        type_t *t = find_struct(tag);
        if (!t) { cc_error("unknown struct tag"); return ty_int(); }
        return t;
    }

    tk = tk->next;                                  /* consume "{" */
    type_t *st = (type_t *)cc_alloc(sizeof(type_t));
    st->kind = TY_STRUCT; st->base = 0; st->arraylen = 0; st->members = 0; st->size = 0;
    if (tag) register_struct(tag, st);              /* register early for self-reference */

    member_t mhead; mhead.next = 0; member_t *mc = &mhead;
    int off = 0;
    while (!is_punct(tk, "}") && tk->kind != TK_EOF) {
        type_t *mty = declspec();
        if (tk->kind != TK_IDENT) { cc_error("expected member name"); break; }
        char *mname = tk->text; tk = tk->next;
        if (consume("[")) {
            if (tk->kind != TK_NUM) cc_error("array length must be a constant");
            int len = (int)tk->val; tk = tk->next; expect("]");
            mty = ty_array_of(mty, len);
        }
        expect(";");
        int al = (mty->size >= 8) ? 8 : 1;          /* 8-byte members must be 8-aligned (MMU off) */
        off = p_align_up(off, al);
        member_t *m = (member_t *)cc_alloc(sizeof(member_t));
        m->name = mname; m->ty = mty; m->offset = off; m->next = 0;
        mc = mc->next = m;
        off += mty->size;
    }
    expect("}");
    st->members = mhead.next;
    st->size = p_align_up(off, 8);
    return st;
}

static int looks_like_type(token_t *t)
{
    return is_kw(t, "int") || is_kw(t, "char") || is_kw(t, "struct")
        || (t && t->kind == TK_IDENT && find_typedef(t->text));
}

/* primary = num | str | ident | ident "(" args ")" | "(" expr ")" */
static node_t *primary(void)
{
    if (consume("(")) { node_t *n = expr(); expect(")"); return n; }

    if (tk->kind == TK_NUM) { node_t *n = new_num(tk->val); tk = tk->next; return n; }

    if (tk->kind == TK_STR) {
        node_t *n = new_node(ND_STR);
        n->str = tk->str; n->slen = tk->slen;
        n->ty = ty_ptr_to(ty_char());
        tk = tk->next;
        return n;
    }

    if (tk->kind == TK_IDENT) {
        char *name = tk->text;
        tk = tk->next;
        if (consume("(")) {                            /* function call */
            node_t *call = new_node(ND_FUNCALL);
            call->funcname = name; call->ty = ty_int();
            node_t arghead; arghead.next = 0; node_t *ac = &arghead;
            if (!is_punct(tk, ")")) {
                ac = ac->next = assign();
                while (consume(",")) ac = ac->next = assign();
            }
            call->args = arghead.next;
            expect(")");
            return call;
        }
        var_t *v = find_var(name);                     /* variable */
        if (!v) { cc_error("undefined variable"); return new_num(0); }
        node_t *n = new_node(ND_VAR); n->var = v; n->ty = v->ty;
        return n;
    }

    cc_error("expected expression");
    return new_num(0);
}

/* postfix = primary ("[" expr "]" | "." ident | "->" ident)* */
static node_t *postfix(void)
{
    node_t *n = primary();
    for (;;) {
        if (is_punct(tk, "[")) {
            tk = tk->next;
            node_t *idx = expr();
            expect("]");
            node_t *add = new_add(n, idx);             /* *(n + idx) */
            node_t *d = new_node(ND_DEREF); d->lhs = add;
            d->ty = add->ty && add->ty->base ? add->ty->base : ty_int();
            n = d;
            continue;
        }
        if (is_punct(tk, ".")) {
            tk = tk->next;
            if (tk->kind != TK_IDENT) { cc_error("expected member name after '.'"); return n; }
            member_t *m = find_member(n->ty, tk->text);
            tk = tk->next;
            if (!m) { cc_error("no such struct member"); return n; }
            node_t *mem = new_node(ND_MEMBER);
            mem->lhs = n; mem->member_off = m->offset; mem->ty = m->ty;
            n = mem;
            continue;
        }
        if (is_punct(tk, "->")) {
            tk = tk->next;
            if (tk->kind != TK_IDENT) { cc_error("expected member name after '->'"); return n; }
            type_t *st = (n->ty && n->ty->kind == TY_PTR) ? n->ty->base : 0;
            member_t *m = find_member(st, tk->text);
            tk = tk->next;
            if (!m) { cc_error("no such struct member (->)"); return n; }
            node_t *deref = new_node(ND_DEREF); deref->lhs = n; deref->ty = st;
            node_t *mem = new_node(ND_MEMBER);
            mem->lhs = deref; mem->member_off = m->offset; mem->ty = m->ty;
            n = mem;
            continue;
        }
        return n;
    }
}

/* unary = "sizeof" ("(" type ")" | unary) | ("+"|"-"|"!"|"*"|"&") unary | postfix */
static node_t *unary(void)
{
    if (consume("sizeof")) {
        if (is_punct(tk, "(") && looks_like_type(tk->next)) {
            tk = tk->next;                       /* consume "(" */
            type_t *t = declspec();
            expect(")");
            return new_num(t->size);
        }
        node_t *e = unary();
        return new_num(e->ty ? e->ty->size : 8);
    }
    if (consume("+")) return unary();
    if (consume("-")) { node_t *n = new_node(ND_NEG); n->lhs = unary(); n->ty = ty_int(); return n; }
    if (consume("!")) { node_t *n = new_node(ND_NOT); n->lhs = unary(); n->ty = ty_int(); return n; }
    if (consume("*")) {
        node_t *n = new_node(ND_DEREF); n->lhs = unary();
        type_t *bt = n->lhs->ty;
        if (!ty_is_ptrlike(bt)) { cc_error("dereference of non-pointer"); n->ty = ty_int(); }
        else n->ty = bt->base;
        return n;
    }
    if (consume("&")) {
        node_t *n = new_node(ND_ADDR); n->lhs = unary();
        n->ty = ty_ptr_to(n->lhs->ty);
        return n;
    }
    return postfix();
}

/* mul = unary ("*"|"/"|"%" unary)* */
static node_t *mul(void)
{
    node_t *n = unary();
    for (;;) {
        if (consume("*")) n = new_binary(ND_MUL, n, unary());
        else if (consume("/")) n = new_binary(ND_DIV, n, unary());
        else if (consume("%")) n = new_binary(ND_MOD, n, unary());
        else return n;
    }
}

/* add = mul ("+"|"-" mul)* */
static node_t *add_expr(void)
{
    node_t *n = mul();
    for (;;) {
        if (consume("+")) n = new_add(n, mul());
        else if (consume("-")) n = new_sub(n, mul());
        else return n;
    }
}

/* relational = add ("<"|"<="|">"|">=" add)* */
static node_t *relational(void)
{
    node_t *n = add_expr();
    for (;;) {
        if (consume("<"))       n = new_binary(ND_LT, n, add_expr());
        else if (consume("<=")) n = new_binary(ND_LE, n, add_expr());
        else if (consume(">"))  n = new_binary(ND_LT, add_expr(), n);   /* a>b == b<a */
        else if (consume(">=")) n = new_binary(ND_LE, add_expr(), n);
        else return n;
    }
}

/* equality = relational ("=="|"!=" relational)* */
static node_t *equality(void)
{
    node_t *n = relational();
    for (;;) {
        if (consume("=="))      n = new_binary(ND_EQ, n, relational());
        else if (consume("!=")) n = new_binary(ND_NE, n, relational());
        else return n;
    }
}

static node_t *logand(void)
{
    node_t *n = equality();
    while (consume("&&")) n = new_binary(ND_LOGAND, n, equality());
    return n;
}

static node_t *logor(void)
{
    node_t *n = logand();
    while (consume("||")) n = new_binary(ND_LOGOR, n, logand());
    return n;
}

/* assign = logor ("=" assign)? */
static node_t *assign(void)
{
    node_t *n = logor();
    if (consume("=")) {
        node_t *a = new_node(ND_ASSIGN); a->lhs = n; a->rhs = assign(); a->ty = n->ty;
        return a;
    }
    return n;
}

static node_t *expr(void) { return assign(); }

static node_t *expr_stmt(void)
{
    node_t *n = new_node(ND_EXPR_STMT);
    n->lhs = expr();
    expect(";");
    return n;
}

/* declaration = declspec ident ("[" num "]")? ("=" expr)? ";" */
static node_t *declaration(void)
{
    type_t *base = declspec();
    char *name = tk->kind == TK_IDENT ? tk->text : 0;
    if (!name) { cc_error("expected identifier in declaration"); return new_node(ND_BLOCK); }
    tk = tk->next;

    type_t *ty = base;
    if (consume("[")) {
        if (tk->kind != TK_NUM) { cc_error("array length must be a constant"); }
        int len = (int)tk->val; tk = tk->next;
        expect("]");
        ty = ty_array_of(base, len);
    }
    var_t *v = new_local(name, ty);

    node_t *n = new_node(ND_BLOCK);                    /* possibly-empty init wrapper */
    if (consume("=")) {
        node_t *lhs = new_node(ND_VAR); lhs->var = v; lhs->ty = v->ty;
        node_t *as  = new_node(ND_ASSIGN); as->lhs = lhs; as->rhs = assign(); as->ty = v->ty;
        node_t *es  = new_node(ND_EXPR_STMT); es->lhs = as;
        n->body = es;
    }
    expect(";");
    return n;
}

static node_t *stmt(void)
{
    if (consume("return")) {
        node_t *n = new_node(ND_RETURN); n->lhs = expr(); expect(";");
        return n;
    }
    if (consume("if")) {
        node_t *n = new_node(ND_IF);
        expect("("); n->cond = expr(); expect(")");
        n->then = stmt();
        if (consume("else")) n->els = stmt();
        return n;
    }
    if (consume("while")) {
        node_t *n = new_node(ND_WHILE);
        expect("("); n->cond = expr(); expect(")");
        n->body = stmt();
        return n;
    }
    if (consume("for")) {
        node_t *n = new_node(ND_FOR);
        expect("(");
        if (!consume(";")) { n->init = new_node(ND_EXPR_STMT); n->init->lhs = expr(); expect(";"); }
        if (!is_punct(tk, ";")) n->cond = expr();
        expect(";");
        if (!is_punct(tk, ")")) n->inc = expr();
        expect(")");
        n->body = stmt();
        return n;
    }
    if (consume("switch")) {
        node_t *n = new_node(ND_SWITCH);
        expect("("); n->cond = expr(); expect(")");
        node_t *prev = cur_switch; cur_switch = n;
        n->cases = 0;
        n->then = stmt();
        cur_switch = prev;
        return n;
    }
    if (consume("case")) {
        if (!cur_switch) { cc_error("case outside switch"); }
        long v = 0; int neg = 0;
        if (is_punct(tk, "-")) { neg = 1; tk = tk->next; }
        if (tk->kind != TK_NUM) { cc_error("case label must be an integer constant"); }
        else { v = neg ? -tk->val : tk->val; tk = tk->next; }
        expect(":");
        node_t *c = new_node(ND_CASE); c->val = v; c->is_default = 0;
        if (cur_switch) { c->case_next = cur_switch->cases; cur_switch->cases = c; }
        c->lhs = stmt();
        return c;
    }
    if (consume("default")) {
        if (!cur_switch) { cc_error("default outside switch"); }
        expect(":");
        node_t *c = new_node(ND_CASE); c->is_default = 1;
        if (cur_switch) { c->case_next = cur_switch->cases; cur_switch->cases = c; }
        c->lhs = stmt();
        return c;
    }
    if (consume("break"))    { expect(";"); return new_node(ND_BREAK); }
    if (consume("continue")) { expect(";"); return new_node(ND_CONTINUE); }

    if (is_punct(tk, "{")) {
        tk = tk->next;
        node_t *blk = new_node(ND_BLOCK);
        node_t head; head.next = 0; node_t *c = &head;
        while (!is_punct(tk, "}") && tk->kind != TK_EOF) {
            node_t *s = looks_like_type(tk) ? declaration() : stmt();
            c = c->next = s;
        }
        expect("}");
        blk->body = head.next;
        return blk;
    }
    return expr_stmt();
}

/* function body: parse "(" params ")" "{ ... }" given the already-read
 * return type and name. */
static func_t *function_rest(type_t *ret, char *name)
{
    func_t *fn = (func_t *)cc_alloc(sizeof(func_t));
    fn->name = name; fn->ret = ret; fn->nparams = 0;
    fn->locals = 0; fn->stacksize = 0; fn->code_off = 0; fn->body = 0; fn->next = 0;

    cur_locals = 0;

    expect("(");
    int np = 0;
    if (!is_punct(tk, ")")) {
        for (;;) {
            type_t *pty = declspec();
            if (tk->kind != TK_IDENT) { cc_error("expected parameter name"); break; }
            char *pname = tk->text; tk = tk->next;
            var_t *v = new_local(pname, pty);   /* prepends to cur_locals */
            if (np < 8) fn->paramv[np] = v;
            np++;
            if (!consume(",")) break;
        }
    }
    expect(")");
    if (np > 8) { cc_error("too many parameters (max 8)"); np = 8; }
    fn->nparams = np;

    if (!is_punct(tk, "{")) { cc_error("expected function body"); return fn; }
    fn->body = stmt();                 /* the "{ ... }" block */
    fn->locals = cur_locals;
    return fn;
}

/* global declaration given the already-read base type and name:
 *   ("[" num "]")? ("=" const)? ";" */
static void global_decl(type_t *base, char *name)
{
    type_t *ty = base;
    if (consume("[")) {
        if (tk->kind != TK_NUM) { cc_error("array length must be a constant"); }
        int len = (int)tk->val; tk = tk->next;
        expect("]");
        ty = ty_array_of(base, len);
    }
    var_t *v = new_global(name, ty);
    if (consume("=")) {
        long val = 0; int neg = 0;
        if (is_punct(tk, "-")) { neg = 1; tk = tk->next; }
        if (tk->kind != TK_NUM) { cc_error("global initializer must be an integer constant"); }
        else { val = neg ? -tk->val : tk->val; tk = tk->next; }
        if (ty->size == 1) *(char *)v->gaddr = (char)val;
        else               *(long *)v->gaddr = val;
    }
    expect(";");
}

func_t *cc_parse(token_t *toks)
{
    tk = toks; g_globals = 0; g_structs = 0; g_typedefs = 0;
    func_t head; head.next = 0; func_t *c = &head;

    while (tk && tk->kind != TK_EOF) {
        if (consume("typedef")) {                /* typedef <type> <name> ; */
            type_t *t = declspec();
            if (tk->kind != TK_IDENT) { cc_error("expected typedef name"); return 0; }
            register_typedef(tk->text, t); tk = tk->next;
            expect(";");
            continue;
        }
        type_t *base = declspec();
        if (consume(";")) continue;     /* bare "struct Foo { ... };" definition */
        if (tk->kind != TK_IDENT) { cc_error("expected identifier at top level"); return 0; }
        char *name = tk->text; tk = tk->next;

        if (is_punct(tk, "(")) {
            func_t *fn = function_rest(base, name);
            if (cc_failed()) return 0;
            if (fn) c = c->next = fn;
        } else {
            global_decl(base, name);
            if (cc_failed()) return 0;
        }
    }
    return head.next;
}
