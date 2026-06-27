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
.global dtb_addr

// ============================================================
// Linux ARM64 Image header  (Documentation/arm64/booting.rst)
// ============================================================
// Required by Pi 5 firmware: the EEPROM bootloader refuses to
// jump to `kernel_2712.img` unless it sees the magic "ARM\x64"
// at offset 0x38 of the file (i.e. of this header).  Without
// the header the firmware leaves its rainbow boot pattern on
// HDMI and our code never runs — symptom: "rainbow unchanged
// even with the early-paint diagnostic in place".
//
// Field layout (every field is little-endian):
//   0x00  code0       4   first executable instruction
//   0x04  code1       4   second executable instruction
//   0x08  text_offset 8   load offset from image base (0)
//   0x10  image_size  8   effective image size hint (1 MiB)
//   0x18  flags       8   bit0=BE, bit1:2=page size, bit3=anywhere
//   0x20  res2        8
//   0x28  res3        8
//   0x30  res4        8
//   0x38  magic       4   ASCII "ARM\x64"
//   0x3c  res5        4   PE-COFF header offset (0 — no PE)
// Total: 64 bytes; real code starts at _real_start below.
_start:
    b       _real_start             // 0x00 code0 — jump over header
    .word   0                       // 0x04 code1 — unused (padding)
    .quad   0x80000                 // 0x08 text_offset (matches link.ld)
    .quad   0x100000                // 0x10 image_size = 1 MiB hint
    .quad   2                       // 0x18 flags (4K pages, LE)
    .quad   0                       // 0x20 res2
    .quad   0                       // 0x28 res3
    .quad   0                       // 0x30 res4
    .ascii  "ARM\x64"               // 0x38 magic
    .word   0                       // 0x3c res5 (no PE header)

_real_start:
    // ---- (1) pin secondary cores ---------------------------------
    // Core id: DynamIQ (Pi 5 A76) sets MPIDR.MT=1 with the core in Aff1
    // (bits 15:8 — cpu@c reg = 0x100*c); a plain cluster (Pi 4 A72) has
    // MT=0 with the core in Aff0 (bits 7:0).  Pick the right field.
    mrs     x1, mpidr_el1
    tst     x1, #(1 << 24)          // MPIDR.MT?
    lsr     x9, x1, #8
    and     x9, x9, #0xff           // Aff1
    and     x1, x1, #0xff           // Aff0
    csel    x1, x9, x1, ne          // MT -> Aff1, else Aff0
    and     x1, x1, #3
    cbz     x1, 2f

    // ---- secondary cores (1-3): wait for core 0 to publish a start
    //      address in smp_release[core] (set by smp_init), then converge
    //      on _smp_start.  On Pi 5 the firmware armstub usually holds cores
    //      1-3 in its spin-table (phys 0xe0/0xe8/0xf0) so they never reach
    //      here; smp_init() also writes that mailbox, jumping them straight
    //      to _smp_start.  Either path converges on _smp_start.
.Lsmp_park:
    wfe
    ldr     x2, =smp_release
    ldr     x3, [x2, x1, lsl #3]
    cbz     x3, .Lsmp_park
    b       _smp_start

    // ---- (1b) drop EL2 -> EL1 if firmware left us at EL2 --------
    //
    // Pi 4 64-bit firmware actually drops to EL2 (confirmed: /ticks
    // reported EL=2), but the rest of the kernel (VBAR_EL1, the EL1
    // physical timer, the GIC config) assumes EL1 — so the 100 Hz
    // timer IRQ asserted (CNTP ISTATUS=1) but was never taken.  Do
    // the standard EL2->EL1 transition.  QEMU virt boots at EL1, so
    // this is skipped there (no behaviour change).
2:  mov     x19, x0                  // preserve firmware DTB ptr across the drop
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    el1_entry                // already EL1 -> nothing to do

    mrs     x0, hcr_el2
    orr     x0, x0, #(1 << 31)       // HCR_EL2.RW = 1: EL1 is AArch64
    msr     hcr_el2, x0

    mrs     x0, cnthctl_el2
    orr     x0, x0, #3               // EL1PCTEN|EL1PCEN: EL1 may use phys timer/counter
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    mov     x0, #0x0800              // SCTLR_EL1 sane reset (MMU/caches off; mmu_init enables later)
    movk    x0, #0x30d0, lsl #16
    msr     sctlr_el1, x0

    mov     x0, #0x3c5               // SPSR_EL2: EL1h, DAIF all masked (kernel unmasks I later)
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0
    eret

    // ---- (2) primary core (now at EL1): set up stack ------------
    //
    // x19 holds the firmware DTB phys addr; stash it across BSS clear,
    // then write it into the .data global `dtb_addr`.
el1_entry:
    mov     x0, x19                  // restore DTB ptr
    ldr     x1, =_start
    mov     sp, x1
    stp     x0, xzr, [sp, #-16]!     // preserve x0 across bss clear

    // ---- (3) zero BSS  (8 bytes per iter, __bss_size is in dwords)
    ldr     x1, =__bss_start
    ldr     w2, =__bss_size
3:  cbz     w2, 4f
    str     xzr, [x1], #8
    sub     w2, w2, #1
    cbnz    w2, 3b

4:  ldp     x0, xzr, [sp], #16       // restore x0
    ldr     x1, =dtb_addr
    str     x0, [x1]

    // ---- (5) jump to C ------------------------------------------
    bl      kernel_main
.Lprimary_halt:
    wfe
    b       .Lprimary_halt

// ============================================================
// _smp_start — a released secondary core (1-3) lands here, from either
// .Lsmp_park (this image parked it) or the firmware spin-table mailbox
// (smp_init wrote _smp_start's address there).  Drop EL2->EL1 like the
// primary, set a per-core stack, and enter the C worker entry.
// ============================================================
.global _smp_start
_smp_start:
    // Core id from MPIDR — Aff1 on DynamIQ (Pi 5), Aff0 on a plain cluster.
    mrs     x20, mpidr_el1
    tst     x20, #(1 << 24)         // MPIDR.MT?
    lsr     x21, x20, #8
    and     x21, x21, #0xff         // Aff1
    and     x20, x20, #0xff         // Aff0
    csel    x20, x21, x20, ne       // MT -> Aff1, else Aff0
    and     x20, x20, #3
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    .Lsmp_el1                // already EL1 (e.g. QEMU)
    mrs     x0, hcr_el2
    orr     x0, x0, #(1 << 31)       // HCR_EL2.RW = 1: EL1 is AArch64
    msr     hcr_el2, x0
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mov     x0, #0x0800              // SCTLR_EL1 sane reset (MMU/caches off)
    movk    x0, #0x30d0, lsl #16
    msr     sctlr_el1, x0
    mov     x0, #0x3c5               // SPSR_EL2: EL1h, DAIF masked
    msr     spsr_el2, x0
    adr     x0, .Lsmp_el1
    msr     elr_el2, x0
    eret
.Lsmp_el1:
    ldr     x2, =smp_stacktop
    ldr     x4, [x2, x20, lsl #3]    // per-core stack top
    mov     sp, x4
    mov     x0, x20                  // arg0 = core id
    bl      smp_secondary_entry
.Lsmp_dead:
    wfe
    b       .Lsmp_dead

// .data so we don't get wiped by the BSS-clear loop.  Initialised
// to zero — boot.S writes the real value after BSS clear.
.section ".data"
.align 3
dtb_addr:
    .quad   0

// SMP handoff tables (in .data so they read as 0 from image load, before
// core 0 clears .bss — a secondary polling smp_release must never observe a
// stale non-zero entry).  Indexed by MPIDR_EL1[1:0]; entry 0 (core 0) unused.
.global smp_release
.global smp_stacktop
.align 3
smp_release:
    .quad   0, 0, 0, 0
smp_stacktop:
    .quad   0, 0, 0, 0
