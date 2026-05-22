// kernel/boot.S — AArch64 boot stub for Raspberry Pi 5 (BCM2712).
//
// Bootstrapped from github.com/radlyeel/leex/kernel/boot.S (Pi 4 / BCM2711
// AArch64 entry).  Pi 5 firmware loads kernel_2712.img at 0x80000 with the
// processor already in EL1 (firmware drops out of EL2 for us when
// config.txt does not request otherwise), so we don't have to do the
// EL2->EL1 switch yourself for the first cut.
//
// What this stub does, in order:
//   1. Identify the boot core via MPIDR_EL1[1:0]; halt the other three
//      cores in a WFE loop (we'll wake them later in S0 / S1 work).
//   2. Move the stack pointer to just below _start so the C kernel has
//      a well-defined initial stack growing downward.
//   3. Zero the .bss section so uninitialised globals are 0 as C
//      requires.  __bss_start / __bss_size come from the linker script
//      (kernel/link.ld).
//   4. Branch to the C entry point `kernel_main`.  If kernel_main ever
//      returns we drop into the same WFE loop the secondary cores use.

.section ".text.boot"
.global _start

_start:
    // ---- (1) pin secondary cores ---------------------------------
    mrs     x1, mpidr_el1
    and     x1, x1, #3
    cbz     x1, 2f

1:  wfe
    b       1b

    // ---- (2) primary core: set up stack -------------------------
2:  ldr     x1, =_start
    mov     sp, x1

    // ---- (3) zero BSS  (8 bytes per iter, __bss_size is in dwords)
    ldr     x1, =__bss_start
    ldr     w2, =__bss_size
3:  cbz     w2, 4f
    str     xzr, [x1], #8
    sub     w2, w2, #1
    cbnz    w2, 3b

    // ---- (4) jump to C ------------------------------------------
4:  bl      kernel_main
    b       1b
