// kernel/ctxsw.S — AArch64 cooperative context switch.
//
//   void ctxsw(void **old_sp, void *new_sp);
//                 ^x0          ^x1
//
// AAPCS64 callee-saved general-purpose registers are x19-x28 plus
// x29 (FP) and x30 (LR).  We're built with -mgeneral-regs-only, so
// there are no v0-v31 to spill.  Saving these onto the current
// kernel stack is all we need for cooperative switching: every
// caller-saved register is already dead at the C call boundary,
// and the SP itself is what we store in *old_sp / load from new_sp.
//
// Layout written here MUST match proc_create()'s hand-built stack
// frame: 12 quadwords (96 bytes), with x29 at the lowest address
// and x30 at the next, so `ret` jumps to the saved LR.

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
    stp     x29, x30, [sp, #-16]!     // last push -> sp+0 = x29, sp+8 = x30

    mov     x2, sp
    str     x2, [x0]                  // *old_sp = current SP

    mov     sp, x1                    // sp = new_sp

    ldp     x29, x30, [sp], #16
    ldp     x27, x28, [sp], #16
    ldp     x25, x26, [sp], #16
    ldp     x23, x24, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x19, x20, [sp], #16

    ret                                // jump to restored LR
    .size ctxsw, .-ctxsw
