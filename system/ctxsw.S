// kernel/ctxsw.S — AArch64 cooperative context switch.
//
//   void ctxsw(void **old_sp, void *new_sp);
//                 ^x0          ^x1
//
// AAPCS64 callee-saved general-purpose registers are x19-x28 plus
// x29 (FP) and x30 (LR).  AAPCS64 callee-saved FP registers are the
// LOWER 64 bits of v8-v15 (= d8-d15).
//
// Most of the kernel is built with -mgeneral-regs-only, so for those
// translation units there is no FP/SIMD state to preserve.  But
// `cc.c` and `llm.c` are built WITHOUT that flag (PI4_FP_CFLAGS in
// the Makefile) because they need floats for the C-subset JIT and
// for transformer inference.  If we yield mid-computation in one of
// those files (e.g. `forward()` in llm.c calling a function that
// eventually ctxsw's) and don't preserve d8-d15, FP math resumes
// with garbage and the worker wedges silently in a converged-NaN
// loop or a wrong-index probe.  Save them.
//
// Layout written here MUST match proc_create()'s hand-built stack
// frame: 16 quadwords (160 bytes), with x29 at the lowest address
// and x30 at the next so `ret` jumps to the saved LR, followed by
// d14/d15/d12/d13/d10/d11/d8/d9, then the remaining callee-saved
// GPRs.  All offsets keep 16-byte SP alignment.

    .section .text
    .global  ctxsw
    .type    ctxsw, %function
    .balign  16
ctxsw:
    stp     x19, x20, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    stp     x23, x24, [sp, #-16]!
    stp     x25, x26, [sp, #-16]!
    stp     x27, x28, [sp, #-16]!
    stp     d8,  d9,  [sp, #-16]!     // AAPCS64 callee-saved FP
    stp     d10, d11, [sp, #-16]!
    stp     d12, d13, [sp, #-16]!
    stp     d14, d15, [sp, #-16]!
    stp     x29, x30, [sp, #-16]!     // last push -> sp+0 = x29, sp+8 = x30

    mov     x2, sp
    str     x2, [x0]                  // *old_sp = current SP

    mov     sp, x1                    // sp = new_sp

    ldp     x29, x30, [sp], #16
    ldp     d14, d15, [sp], #16
    ldp     d12, d13, [sp], #16
    ldp     d10, d11, [sp], #16
    ldp     d8,  d9,  [sp], #16
    ldp     x27, x28, [sp], #16
    ldp     x25, x26, [sp], #16
    ldp     x23, x24, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x19, x20, [sp], #16

    ret                                // jump to restored LR
    .size ctxsw, .-ctxsw
