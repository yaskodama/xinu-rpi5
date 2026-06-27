// system/exception_vectors.S — AArch64 exception vector table.
//
// 16 entries × 0x80 bytes, 2 KiB aligned (VBAR_EL1 spec).  Each
// entry is just `b <C handler>` — handlers don't preserve any
// registers because they dump-and-halt; once S1d brings up real
// IRQ dispatch this file will grow a save/restore prologue.
//
// Layout (per AArch64 ARMv8 Architecture Reference Manual):
//   0x000  Current EL with SP0  — sync
//   0x080  Current EL with SP0  — IRQ
//   0x100  Current EL with SP0  — FIQ
//   0x180  Current EL with SP0  — SError
//   0x200  Current EL with SPx  — sync       <-- our normal sync EL1
//   0x280  Current EL with SPx  — IRQ        <-- our normal IRQ
//   0x300  Current EL with SPx  — FIQ
//   0x380  Current EL with SPx  — SError
//   0x400  Lower EL, AArch64    — sync
//   0x480  Lower EL, AArch64    — IRQ
//   0x500  Lower EL, AArch64    — FIQ
//   0x580  Lower EL, AArch64    — SError
//   0x600  Lower EL, AArch32    — sync
//   0x680  Lower EL, AArch32    — IRQ
//   0x700  Lower EL, AArch32    — FIQ
//   0x780  Lower EL, AArch32    — SError

.section ".text"
.balign 0x800
.global exception_vector_table
exception_vector_table:
    // Current EL with SP0
    b   sync_handler_default
    .balign 0x80
    b   irq_handler_default
    .balign 0x80
    b   fiq_handler_default
    .balign 0x80
    b   serror_handler_default

    // Current EL with SPx  (this is where EL1 normally takes exceptions)
    .balign 0x80
    b   sync_entry                 // <-- recoverable sync dispatch (demand paging)
    .balign 0x80
    b   irq_entry                  // <-- real IRQ dispatch lives here
    .balign 0x80
    b   fiq_handler_default
    .balign 0x80
    b   serror_handler_default

    // Lower EL using AArch64
    .balign 0x80
    b   sync_handler_default
    .balign 0x80
    b   irq_handler_default
    .balign 0x80
    b   fiq_handler_default
    .balign 0x80
    b   serror_handler_default

    // Lower EL using AArch32
    .balign 0x80
    b   sync_handler_default
    .balign 0x80
    b   irq_handler_default
    .balign 0x80
    b   fiq_handler_default
    .balign 0x80
    b   serror_handler_default

// exception_init() — program VBAR_EL1 with the table address and
// ISB so the new vectors take effect before the next instruction.
.global exception_init
exception_init:
    adrp    x0, exception_vector_table
    add     x0, x0, :lo12:exception_vector_table
    msr     vbar_el1, x0
    isb
    ret

// ----------------------------------------------------------------
// irq_entry — AArch64 IRQ exception stub.
//
// Hardware on IRQ entry: saves PC→ELR_EL1, PSTATE→SPSR_EL1, masks
// IRQ in DAIF (we don't unmask inside the handler so nesting is
// blocked).  The vector branched here without BL so x30 still
// holds the interrupted code's value; we save x30 explicitly.
//
// Layout of the saved frame: 32 × 8 = 256 bytes, all GPRs except
// SP itself (which is already where we want it).  Restored in
// reverse, then ERET pops ELR / SPSR back.
// ----------------------------------------------------------------
// ----------------------------------------------------------------
// sync_entry — recoverable synchronous-exception stub (EL1, SPx).
//
// Same save/restore shape as irq_entry, but calls sync_dispatch_c.
// On a demand-paging translation fault the dispatcher maps the page
// and returns; we restore the GPRs and ERET, which re-runs the
// faulting instruction (ELR_EL1 still points at it).  Unhandled
// faults never return — sync_dispatch_c dumps + parks the core.
// ----------------------------------------------------------------
.global sync_entry
sync_entry:
    sub     sp, sp, #(16 * 16)
    stp     x0,  x1,  [sp, #16 *  0]
    stp     x2,  x3,  [sp, #16 *  1]
    stp     x4,  x5,  [sp, #16 *  2]
    stp     x6,  x7,  [sp, #16 *  3]
    stp     x8,  x9,  [sp, #16 *  4]
    stp     x10, x11, [sp, #16 *  5]
    stp     x12, x13, [sp, #16 *  6]
    stp     x14, x15, [sp, #16 *  7]
    stp     x16, x17, [sp, #16 *  8]
    stp     x18, x19, [sp, #16 *  9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #16 * 15]

    bl      sync_dispatch_c

    ldp     x0,  x1,  [sp, #16 *  0]
    ldp     x2,  x3,  [sp, #16 *  1]
    ldp     x4,  x5,  [sp, #16 *  2]
    ldp     x6,  x7,  [sp, #16 *  3]
    ldp     x8,  x9,  [sp, #16 *  4]
    ldp     x10, x11, [sp, #16 *  5]
    ldp     x12, x13, [sp, #16 *  6]
    ldp     x14, x15, [sp, #16 *  7]
    ldp     x16, x17, [sp, #16 *  8]
    ldp     x18, x19, [sp, #16 *  9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #16 * 15]
    add     sp, sp, #(16 * 16)
    eret

.global irq_entry
irq_entry:
    sub     sp, sp, #(16 * 16)
    stp     x0,  x1,  [sp, #16 *  0]
    stp     x2,  x3,  [sp, #16 *  1]
    stp     x4,  x5,  [sp, #16 *  2]
    stp     x6,  x7,  [sp, #16 *  3]
    stp     x8,  x9,  [sp, #16 *  4]
    stp     x10, x11, [sp, #16 *  5]
    stp     x12, x13, [sp, #16 *  6]
    stp     x14, x15, [sp, #16 *  7]
    stp     x16, x17, [sp, #16 *  8]
    stp     x18, x19, [sp, #16 *  9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #16 * 15]

    bl      irq_dispatch_c

    ldp     x0,  x1,  [sp, #16 *  0]
    ldp     x2,  x3,  [sp, #16 *  1]
    ldp     x4,  x5,  [sp, #16 *  2]
    ldp     x6,  x7,  [sp, #16 *  3]
    ldp     x8,  x9,  [sp, #16 *  4]
    ldp     x10, x11, [sp, #16 *  5]
    ldp     x12, x13, [sp, #16 *  6]
    ldp     x14, x15, [sp, #16 *  7]
    ldp     x16, x17, [sp, #16 *  8]
    ldp     x18, x19, [sp, #16 *  9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #16 * 15]
    add     sp, sp, #(16 * 16)
    eret
