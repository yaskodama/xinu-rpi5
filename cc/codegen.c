// cc/codegen.c — AArch64 machine-code generator for the C subset.
//
// Strategy: a simple stack machine.  Every expression computes its
// result into x0; binary operators save the right operand on the
// machine stack (SP, 16-byte slots) while the left is computed, then
// pop it back.  Local variables live in the function's stack frame at
// fixed offsets below x29 (the frame pointer).  Because we JIT directly
// into the buffer that will execute, every absolute address (string
// literals, called function entries, kernel builtins) is known at emit
// time and baked in with a movz/movk sequence — no relocation needed.
//
// All instruction encodings were taken from the assembler (verified
// with aarch64-elf-as) so the hand-emitted words match exactly.

#include "ccpriv.h"

/* ---------- emit buffer ---------- */

static unsigned char *g_buf;
static int            g_pos, g_cap, g_oom;
static func_t        *g_funcs;

static void put32(int pos, unsigned int w)
{
    g_buf[pos + 0] = (unsigned char)(w);
    g_buf[pos + 1] = (unsigned char)(w >> 8);
    g_buf[pos + 2] = (unsigned char)(w >> 16);
    g_buf[pos + 3] = (unsigned char)(w >> 24);
}

static void emit(unsigned int w)
{
    if (g_pos + 4 > g_cap) { g_oom = 1; return; }
    put32(g_pos, w);
    g_pos += 4;
}

/* ---------- register-parameterised encoders ---------- */

#define X9  9
#define X29 29
#define SP  31
#define XZR 31

static void emit_push(void)        { emit(0xF81F0FE0u); }            /* str x0,[sp,#-16]! */
static void emit_pop(int r)        { emit(0xF84107E0u | (unsigned)r); } /* ldr xr,[sp],#16 */

static void emit_add_rrr(int d,int n,int m){ emit(0x8B000000u|((unsigned)m<<16)|((unsigned)n<<5)|(unsigned)d); }
static void emit_sub_rrr(int d,int n,int m){ emit(0xCB000000u|((unsigned)m<<16)|((unsigned)n<<5)|(unsigned)d); }
static void emit_mul(int d,int n,int m)    { emit(0x9B000000u|((unsigned)m<<16)|(0x1Fu<<10)|((unsigned)n<<5)|(unsigned)d); }
static void emit_sdiv(int d,int n,int m)   { emit(0x9AC00C00u|((unsigned)m<<16)|((unsigned)n<<5)|(unsigned)d); }
static void emit_msub(int d,int n,int m,int a){ emit(0x9B008000u|((unsigned)m<<16)|((unsigned)a<<10)|((unsigned)n<<5)|(unsigned)d); }

static void emit_add_imm(int d,int n,int imm){ emit(0x91000000u|(((unsigned)imm&0xFFF)<<10)|((unsigned)n<<5)|(unsigned)d); }
static void emit_sub_imm(int d,int n,int imm){ emit(0xD1000000u|(((unsigned)imm&0xFFF)<<10)|((unsigned)n<<5)|(unsigned)d); }

static void emit_cmp_rr(int n,int m){ emit(0xEB000000u|((unsigned)m<<16)|((unsigned)n<<5)|0x1Fu); }
static void emit_cmp0(int n)        { emit(0xF100001Fu|((unsigned)n<<5)); }   /* cmp xn,#0 */

/* cset xd, cond : CSINC with inverted condition (cond^1) in bits 15:12 */
enum { C_EQ=0, C_NE=1, C_LT=11, C_LE=13 };
static void emit_cset(int d,int cond){ emit(0x9A9F07E0u|((unsigned)(cond^1)<<12)|(unsigned)d); }

static void emit_ldr(int t,int n)  { emit(0xF9400000u|((unsigned)n<<5)|(unsigned)t); }  /* ldr xt,[xn] */
static void emit_str(int t,int n)  { emit(0xF9000000u|((unsigned)n<<5)|(unsigned)t); }  /* str xt,[xn] */
static void emit_ldrb(int t,int n) { emit(0x39400000u|((unsigned)n<<5)|(unsigned)t); }  /* ldrb wt,[xn] */
static void emit_strb(int t,int n) { emit(0x39000000u|((unsigned)n<<5)|(unsigned)t); }  /* strb wt,[xn] */

static void emit_mov_imm_small(int d,unsigned v){ emit(0xD2800000u|((v&0xFFFF)<<5)|(unsigned)d); } /* movz */

static void emit_blr(int n){ emit(0xD63F0000u|((unsigned)n<<5)); }
static void emit_ret(void) { emit(0xD65F03C0u); }
static void emit_prologue_stp(void){ emit(0xA9BF7BFDu); }          /* stp x29,x30,[sp,#-16]! */
static void emit_mov_fp_sp(void)   { emit(0x910003FDu); }          /* mov x29,sp */
static void emit_mov_sp_fp(void)   { emit(0x910003BFu); }          /* mov sp,x29 */
static void emit_epilogue_ldp(void){ emit(0xA8C17BFDu); }          /* ldp x29,x30,[sp],#16 */

/* load a 64-bit constant into reg via movz + 3×movk */
static void emit_loadimm(int reg, unsigned long v)
{
    emit(0xD2800000u | ((unsigned)( v        & 0xFFFF) << 5) | (unsigned)reg);
    emit(0xF2800000u | (1u<<21) | ((unsigned)((v >> 16) & 0xFFFF) << 5) | (unsigned)reg);
    emit(0xF2800000u | (2u<<21) | ((unsigned)((v >> 32) & 0xFFFF) << 5) | (unsigned)reg);
    emit(0xF2800000u | (3u<<21) | ((unsigned)((v >> 48) & 0xFFFF) << 5) | (unsigned)reg);
}
static void patch_loadimm(int pos, int reg, unsigned long v)
{
    put32(pos + 0,  0xD2800000u | ((unsigned)( v        & 0xFFFF) << 5) | (unsigned)reg);
    put32(pos + 4,  0xF2800000u | (1u<<21) | ((unsigned)((v >> 16) & 0xFFFF) << 5) | (unsigned)reg);
    put32(pos + 8,  0xF2800000u | (2u<<21) | ((unsigned)((v >> 32) & 0xFFFF) << 5) | (unsigned)reg);
    put32(pos + 12, 0xF2800000u | (3u<<21) | ((unsigned)((v >> 48) & 0xFFFF) << 5) | (unsigned)reg);
}

/* ---------- labels + branch fixups (per function) ---------- */

#define MAXLAB 2048
#define MAXBFX 2048
#define MAXCFX 2048

static int  lab_off[MAXLAB];
static int  nlab;

enum { BR_B, BR_CBZ, BR_CBNZ, BR_BCOND };
static struct { int site; int lab; int kind; int cond; } bfx[MAXBFX];
static int  nbfx;

/* break / continue target stacks (per function) */
#define LOOPDEPTH 64
static int brk_lab[LOOPDEPTH], cont_lab[LOOPDEPTH];
static int brk_top, cont_top;

/* program-wide forward references to user functions */
static struct { int site; func_t *fn; } cfx[MAXCFX];
static int  ncfx;

static int newlabel(void){ if (nlab >= MAXLAB) { cc_error("too many labels"); return 0; } return nlab++; }
static void place(int l){ if (l < MAXLAB) lab_off[l] = g_pos; }

static void emit_b(int l)   { if (nbfx<MAXBFX){bfx[nbfx].site=g_pos;bfx[nbfx].lab=l;bfx[nbfx].kind=BR_B;nbfx++;} emit(0x14000000u); }
static void emit_cbz(int l) { if (nbfx<MAXBFX){bfx[nbfx].site=g_pos;bfx[nbfx].lab=l;bfx[nbfx].kind=BR_CBZ;nbfx++;} emit(0xB4000000u); }
static void emit_bcond(int cond, int l) { if (nbfx<MAXBFX){bfx[nbfx].site=g_pos;bfx[nbfx].lab=l;bfx[nbfx].kind=BR_BCOND;bfx[nbfx].cond=cond;nbfx++;} emit(0x54000000u); }

static void resolve_branches(void)
{
    for (int i = 0; i < nbfx; i++) {
        int delta = (lab_off[bfx[i].lab] - bfx[i].site) >> 2;
        unsigned w;
        if (bfx[i].kind == BR_B)          w = 0x14000000u | ((unsigned)delta & 0x03FFFFFFu);
        else if (bfx[i].kind == BR_CBZ)   w = 0xB4000000u | (((unsigned)delta & 0x7FFFFu) << 5);
        else if (bfx[i].kind == BR_CBNZ)  w = 0xB5000000u | (((unsigned)delta & 0x7FFFFu) << 5);
        else /* BR_BCOND */               w = 0x54000000u | (((unsigned)delta & 0x7FFFFu) << 5) | (unsigned)bfx[i].cond;
        put32(bfx[i].site, w);
    }
}

/* ---------- function table ---------- */

static func_t *find_func(const char *name)
{
    for (func_t *f = g_funcs; f; f = f->next) {
        const char *a = f->name, *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return f;
    }
    return 0;
}

/* ---------- expression / statement codegen ---------- */

static int           cur_lret;   /* epilogue label of the current function   */
static unsigned long g_tickaddr;  /* &cc_tick, for the runaway-loop guard     */

static void gen_expr(node_t *n);
static void gen_stmt(node_t *n);

/* Runaway guard: call cc_tick(); if it returns 0 (budget spent) branch to
 * `exit_label` so the loop terminates instead of hanging the device. */
static void emit_budget_check(int exit_label)
{
    if (!g_tickaddr) return;
    emit_loadimm(X9, g_tickaddr);
    emit_blr(X9);
    emit_cbz(exit_label);
}

/* compute the address of an lvalue into x0 */
static void gen_addr(node_t *n)
{
    switch (n->kind) {
        case ND_VAR:
            if (n->var->is_global) {
                emit_loadimm(0, n->var->gaddr);           /* x0 = &global */
            } else if (n->var->offset <= 0xFFF) {
                emit_sub_imm(0, X29, n->var->offset);     /* x0 = x29 - off */
            } else {
                emit_loadimm(X9, (unsigned long)n->var->offset);
                emit_sub_rrr(0, X29, X9);
            }
            return;
        case ND_DEREF:
            gen_expr(n->lhs);                              /* pointer value = address */
            return;
        case ND_MEMBER:
            gen_addr(n->lhs);
            if (n->member_off) emit_add_imm(0, 0, n->member_off);
            return;
        case ND_STR:
            emit_loadimm(0, (unsigned long)n->str);
            return;
        default:
            cc_error("not an lvalue");
            return;
    }
}

/* load the value at [x0] into x0, honouring the operand type */
static void gen_load(type_t *ty)
{
    if (ty && ty->kind == TY_ARRAY) return;               /* arrays decay to address */
    if (ty && ty->size == 1) emit_ldrb(0, 0);
    else                     emit_ldr(0, 0);
}

/* store x0 to [x1], honouring the destination type */
static void gen_store(type_t *ty)
{
    if (ty && ty->size == 1) emit_strb(0, 1);
    else                     emit_str(0, 1);
}

static void gen_binary(node_t *n)
{
    gen_expr(n->rhs); emit_push();
    gen_expr(n->lhs); emit_pop(1);     /* x0 = lhs, x1 = rhs */

    switch (n->kind) {
        case ND_ADD: emit_add_rrr(0,0,1); break;
        case ND_SUB: emit_sub_rrr(0,0,1); break;
        case ND_MUL: emit_mul(0,0,1);     break;
        case ND_DIV: emit_sdiv(0,0,1);    break;
        case ND_MOD: emit_sdiv(2,0,1); emit_msub(0,2,1,0); break;  /* x0 = x0 - (x0/x1)*x1 */
        case ND_EQ:  emit_cmp_rr(0,1); emit_cset(0,C_EQ); break;
        case ND_NE:  emit_cmp_rr(0,1); emit_cset(0,C_NE); break;
        case ND_LT:  emit_cmp_rr(0,1); emit_cset(0,C_LT); break;
        case ND_LE:  emit_cmp_rr(0,1); emit_cset(0,C_LE); break;
        default: cc_error("bad binary op"); break;
    }
}

static void gen_call(node_t *n)
{
    node_t *av[8];
    int argc = 0;
    for (node_t *a = n->args; a; a = a->next) {
        if (argc < 8) av[argc] = a;
        argc++;
    }
    if (argc > 8) { cc_error("too many call arguments (max 8)"); argc = 8; }

    for (int i = 0; i < argc; i++) { gen_expr(av[i]); emit_push(); }
    for (int i = argc - 1; i >= 0; i--) emit_pop(i);     /* x0..x{argc-1} */

    func_t *uf = find_func(n->funcname);
    if (uf) {
        if (ncfx < MAXCFX) { cfx[ncfx].site = g_pos; cfx[ncfx].fn = uf; ncfx++; }
        emit_loadimm(X9, 0);                              /* patched later */
        emit_blr(X9);
    } else {
        unsigned long addr = cc_resolve_extern(n->funcname);
        if (!addr) { cc_error("call to undefined function"); return; }
        emit_loadimm(X9, addr);
        emit_blr(X9);
    }
}

static void gen_expr(node_t *n)
{
    if (!n || cc_failed()) return;

    switch (n->kind) {
        case ND_NUM:  emit_loadimm(0, (unsigned long)n->val); return;
        case ND_STR:  emit_loadimm(0, (unsigned long)n->str); return;

        case ND_VAR:
            gen_addr(n);
            gen_load(n->ty);
            return;

        case ND_ASSIGN:
            gen_addr(n->lhs); emit_push();
            gen_expr(n->rhs); emit_pop(1);                /* x1 = address */
            gen_store(n->lhs->ty);
            return;

        case ND_DEREF:
            gen_expr(n->lhs);
            gen_load(n->ty);
            return;

        case ND_MEMBER:
            gen_addr(n);
            gen_load(n->ty);
            return;

        case ND_ADDR:
            gen_addr(n->lhs);
            return;

        case ND_NEG:
            gen_expr(n->lhs);
            emit_sub_rrr(0, XZR, 0);                      /* x0 = 0 - x0 */
            return;

        case ND_NOT:
            gen_expr(n->lhs);
            emit_cmp0(0); emit_cset(0, C_EQ);
            return;

        case ND_LOGAND: {
            int lfalse = newlabel(), lend = newlabel();
            gen_expr(n->lhs); emit_cbz(lfalse);
            gen_expr(n->rhs); emit_cbz(lfalse);
            emit_mov_imm_small(0, 1); emit_b(lend);
            place(lfalse); emit_mov_imm_small(0, 0);
            place(lend);
            return;
        }
        case ND_LOGOR: {
            int ltrue = newlabel(), lend = newlabel();
            /* cbnz via: cbz to skip-true is awkward; use explicit cbnz */
            gen_expr(n->lhs); emit_cmp0(0);
            /* if lhs != 0 jump true */
            if (nbfx<MAXBFX){bfx[nbfx].site=g_pos;bfx[nbfx].lab=ltrue;bfx[nbfx].kind=BR_CBNZ;nbfx++;}
            emit(0xB5000000u);                            /* cbnz x0, ltrue (patched) */
            gen_expr(n->rhs);
            if (nbfx<MAXBFX){bfx[nbfx].site=g_pos;bfx[nbfx].lab=ltrue;bfx[nbfx].kind=BR_CBNZ;nbfx++;}
            emit(0xB5000000u);                            /* cbnz x0, ltrue */
            emit_mov_imm_small(0, 0); emit_b(lend);
            place(ltrue); emit_mov_imm_small(0, 1);
            place(lend);
            return;
        }

        case ND_FUNCALL: gen_call(n); return;

        case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD:
        case ND_EQ:  case ND_NE:  case ND_LT:  case ND_LE:
            gen_binary(n);
            return;

        default:
            cc_error("unexpected node in expression");
            return;
    }
}

static void gen_stmt(node_t *n)
{
    if (!n || cc_failed()) return;

    switch (n->kind) {
        case ND_EXPR_STMT:
            gen_expr(n->lhs);
            return;

        case ND_RETURN:
            gen_expr(n->lhs);
            emit_b(cur_lret);
            return;

        case ND_IF: {
            int lelse = newlabel(), lend = newlabel();
            gen_expr(n->cond); emit_cbz(lelse);
            gen_stmt(n->then); emit_b(lend);
            place(lelse);
            if (n->els) gen_stmt(n->els);
            place(lend);
            return;
        }
        case ND_WHILE: {
            int lbegin = newlabel(), lend = newlabel();
            place(lbegin);
            gen_expr(n->cond); emit_cbz(lend);
            brk_lab[brk_top++] = lend; cont_lab[cont_top++] = lbegin;
            gen_stmt(n->body);
            brk_top--; cont_top--;
            emit_budget_check(lend);
            emit_b(lbegin);
            place(lend);
            return;
        }
        case ND_FOR: {
            int lbegin = newlabel(), lcont = newlabel(), lend = newlabel();
            if (n->init) gen_stmt(n->init);
            place(lbegin);
            if (n->cond) { gen_expr(n->cond); emit_cbz(lend); }
            brk_lab[brk_top++] = lend; cont_lab[cont_top++] = lcont;
            gen_stmt(n->body);
            brk_top--; cont_top--;
            place(lcont);
            if (n->inc) gen_expr(n->inc);
            emit_budget_check(lend);
            emit_b(lbegin);
            place(lend);
            return;
        }
        case ND_SWITCH: {
            int lend = newlabel();
            gen_expr(n->cond);                       /* x0 = switch value */
            node_t *defcase = 0;
            for (node_t *c = n->cases; c; c = c->case_next) {
                c->clabel = newlabel();
                if (c->is_default) { defcase = c; continue; }
                emit_loadimm(1, (unsigned long)c->val);
                emit_cmp_rr(0, 1);
                emit_bcond(C_EQ, c->clabel);
            }
            if (defcase) emit_b(defcase->clabel); else emit_b(lend);
            brk_lab[brk_top++] = lend;               /* continue is NOT captured by switch */
            gen_stmt(n->then);
            brk_top--;
            place(lend);
            return;
        }
        case ND_CASE:
            place(n->clabel);
            gen_stmt(n->lhs);
            return;
        case ND_BREAK:
            if (brk_top > 0) emit_b(brk_lab[brk_top - 1]); else cc_error("break outside loop/switch");
            return;
        case ND_CONTINUE:
            if (cont_top > 0) emit_b(cont_lab[cont_top - 1]); else cc_error("continue outside loop");
            return;
        case ND_BLOCK:
            for (node_t *s = n->body; s; s = s->next) gen_stmt(s);
            return;

        default:
            /* a bare expression node used as a statement */
            gen_expr(n);
            return;
    }
}

/* ---------- per-function frame + emit ---------- */

static int align_up(int n, int a) { return (n + a - 1) / a * a; }

static int assign_offsets(func_t *fn)
{
    int off = 0;
    for (var_t *v = fn->locals; v; v = v->next) {
        off += v->ty->size;
        off = align_up(off, 8);
        v->offset = off;
    }
    return align_up(off, 16);
}

static void gen_func(func_t *fn)
{
    fn->code_off = g_pos;
    int frame = assign_offsets(fn);
    fn->stacksize = frame;

    nlab = 0; nbfx = 0; brk_top = 0; cont_top = 0;
    cur_lret = newlabel();

    emit_prologue_stp();
    emit_mov_fp_sp();
    if (frame > 0) {
        if (frame <= 0xFFF) emit_sub_imm(SP, SP, frame);
        else { emit_loadimm(X9, (unsigned long)frame); emit_sub_rrr(SP, SP, X9); }
    }

    /* spill incoming parameters x0..x{n-1} into their frame slots */
    for (int i = 0; i < fn->nparams; i++) {
        var_t *p = fn->paramv[i];
        if (p->offset <= 0xFFF) emit_sub_imm(X9, X29, p->offset);
        else { emit_loadimm(X9, (unsigned long)p->offset);
               /* x9 = x29 - x9 */ emit_sub_rrr(X9, X29, X9); }
        if (p->ty->size == 1) emit_strb(i, X9);
        else                  emit_str(i, X9);
    }

    gen_stmt(fn->body);

    /* Fall-through default return 0.  Placed BEFORE cur_lret so explicit
     * `return expr` (which branches to cur_lret with x0 already set)
     * keeps its value, while falling off the end yields 0. */
    emit_mov_imm_small(0, 0);
    place(cur_lret);
    emit_mov_sp_fp();
    emit_epilogue_ldp();
    emit_ret();

    resolve_branches();
}

int cc_codegen(func_t *funcs, unsigned char *buf, int cap, int *entry_off)
{
    g_buf = buf; g_pos = 0; g_cap = cap; g_oom = 0;
    g_funcs = funcs; ncfx = 0;
    g_tickaddr = cc_resolve_extern("__cc_tick");

    for (func_t *f = funcs; f; f = f->next) {
        gen_func(f);
        if (cc_failed()) return -1;
        if (g_oom) { cc_error("code buffer overflow"); return -1; }
    }

    /* resolve forward/any user-function calls now that all offsets known */
    for (int i = 0; i < ncfx; i++) {
        unsigned long addr = (unsigned long)g_buf + (unsigned long)cfx[i].fn->code_off;
        patch_loadimm(cfx[i].site, X9, addr);
    }

    func_t *mainf = find_func("main");
    if (!mainf) { cc_error("no main() function"); return -1; }
    *entry_off = mainf->code_off;
    return g_pos;
}

int cc_func_offset(const char *name)
{
    func_t *f = find_func(name);
    return f ? f->code_off : -1;
}
