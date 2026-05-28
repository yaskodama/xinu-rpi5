// cc/ccpriv.h — internal interface shared by the on-device C compiler
// stages (lexer + parser in parse.c, code generator in codegen.c,
// driver + runtime in cc.c).
//
// The compiler targets a deliberate subset of C — int, char, pointers,
// arrays, string literals; +-*/%, comparisons, && || !, & *, assignment;
// if/else/while/for/return/blocks; functions with parameters and
// recursion — and emits AArch64 machine code that is executed in place
// (JIT) on the bare-metal kernel.  Generated code calls back into the
// kernel through an extensible external-symbol table (print/putchar/
// puts/actor_send, ...), which is how translated AIPL behaviour will
// reach the Xinu actor runtime.
//
// Simplification worth remembering: `int` is 8 bytes here (matches the
// AIPL value_t 64-bit tagged word and keeps all register math in the
// 64-bit X registers).  `char` is 1 byte.  Pointers/arrays are 8 bytes.

#ifndef CC_PRIV_H
#define CC_PRIV_H

/* ---------- arena + error reporting ---------- */

void        *cc_alloc(unsigned long n);   /* bump-allocate from the arena   */
void         cc_error(const char *msg);   /* record first error + message   */
void         cc_errorc(const char *msg, char c);
int          cc_failed(void);
const char  *cc_errmsg(void);

/* ---------- tokens ---------- */

typedef enum { TK_NUM, TK_STR, TK_IDENT, TK_PUNCT, TK_KW, TK_EOF } tokkind_t;

typedef struct token {
    tokkind_t      kind;
    long           val;     /* TK_NUM                                    */
    char          *str;     /* TK_STR payload (NUL-terminated copy)      */
    int            slen;    /* TK_STR length (excludes the NUL)          */
    char          *text;    /* IDENT/KW/PUNCT spelling (NUL-terminated)  */
    struct token  *next;
} token_t;

token_t *cc_lex(const char *src);

/* ---------- types ---------- */

typedef enum { TY_INT, TY_CHAR, TY_PTR, TY_ARRAY, TY_STRUCT } tykind_t;

typedef struct type {
    tykind_t       kind;
    int            size;       /* bytes                                  */
    struct type   *base;       /* PTR / ARRAY element type               */
    int            arraylen;   /* ARRAY                                  */
    struct member *members;    /* STRUCT member list                     */
} type_t;

typedef struct member {
    char           *name;
    type_t         *ty;
    int             offset;
    struct member  *next;
} member_t;

type_t *ty_int(void);
type_t *ty_char(void);
type_t *ty_ptr_to(type_t *base);
type_t *ty_array_of(type_t *base, int len);
int     ty_is_ptrlike(type_t *t);   /* PTR or ARRAY                      */

/* ---------- AST ---------- */

typedef enum {
    ND_NUM, ND_VAR, ND_STR,
    ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD,
    ND_EQ, ND_NE, ND_LT, ND_LE,
    ND_LOGAND, ND_LOGOR, ND_NOT, ND_NEG,
    ND_ASSIGN, ND_ADDR, ND_DEREF, ND_MEMBER,
    ND_FUNCALL,
    ND_RETURN, ND_IF, ND_WHILE, ND_FOR, ND_BLOCK, ND_EXPR_STMT,
    ND_SWITCH, ND_CASE, ND_BREAK, ND_CONTINUE
} nodekind_t;

typedef struct var {
    char         *name;
    type_t       *ty;
    int           offset;    /* bytes below x29 (locals)                 */
    int           is_global; /* 1 = global; address is `gaddr`           */
    unsigned long gaddr;     /* absolute address of a global's storage   */
    struct var   *next;
} var_t;

typedef struct node {
    nodekind_t    kind;
    type_t       *ty;
    struct node  *lhs, *rhs;
    /* control flow */
    struct node  *cond, *then, *els, *init, *inc, *body;
    struct node  *next;      /* block statements / call argument list    */
    /* leaves */
    long          val;       /* ND_NUM                                   */
    var_t        *var;       /* ND_VAR                                   */
    char         *str; int slen;             /* ND_STR                   */
    char         *funcname; struct node *args; /* ND_FUNCALL             */
    int           member_off;                /* ND_MEMBER byte offset    */
    struct node  *cases;                     /* ND_SWITCH: case list     */
    struct node  *case_next;                 /* ND_CASE: next case        */
    int           is_default;                /* ND_CASE: default:         */
    int           clabel;                    /* ND_CASE: codegen label    */
} node_t;

typedef struct func {
    char         *name;
    type_t       *ret;
    var_t        *paramv[8]; /* parameters in source order (max 8 = x0..x7)*/
    int           nparams;
    var_t        *locals;    /* params + body locals (any order)         */
    int           stacksize; /* frame bytes (16-aligned)                 */
    int           code_off;  /* byte offset of entry in the code buffer  */
    node_t       *body;      /* ND_BLOCK                                 */
    struct func  *next;
} func_t;

func_t *cc_parse(token_t *toks);

/* ---------- code generation ---------- */

/* Emit machine code for every function into `buf` (capacity `cap`
 * bytes).  Returns the number of bytes emitted, or -1 on error.  Sets
 * *entry_off to the byte offset of `main` within the buffer. */
int cc_codegen(func_t *funcs, unsigned char *buf, int cap, int *entry_off);

/* Byte offset of function `name` in the buffer last codegen'd, or -1.
 * Valid until the next cc_codegen call. */
int cc_func_offset(const char *name);

/* Resolve a builtin/external name to a kernel function address (0 if
 * unknown).  Defined in cc.c; the table is how compiled code reaches
 * print/putchar/puts/actor_send and, later, the AIPL runtime. */
unsigned long cc_resolve_extern(const char *name);

#endif /* CC_PRIV_H */
