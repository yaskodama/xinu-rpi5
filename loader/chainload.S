// loader/chainload.S — bare-metal chainload trampoline (kexec-style).
//
// Boots a freshly-fetched kernel image (e.g. TFTP'd over WiFi into staging
// RAM) without a power cycle: copy it to the firmware load address 0x80000
// and jump, recreating the firmware's handoff state (MMU off, caches off,
// EL1).  The new kernel's boot.S re-inits everything.
//
// CRITICAL: this stub must run from RAM that does NOT overlap the copy
// destination (0x80000) — the C wrapper relocates it to a safe high address
// and cleans D-cache / invalidates I-cache for the stub + the source region
// (while the MMU is still on) before jumping here.
//
//   x0 = src  (staged kernel image)
//   x1 = dst  (0x80000)
//   x2 = len  (bytes)
//
// No stack use (entered via blr; never returns — br to dst at the end).

.section ".text"
.global chainload_stub
.global chainload_stub_end

chainload_stub:
    mov     x3, x1                  // save dst (x1 is advanced by the copy)

    // ---- disable MMU + D-cache + I-cache (SCTLR_EL1) ----
    mrs     x9, sctlr_el1
    bic     x9, x9, #(1 << 0)       // M  = 0  (MMU off)
    bic     x9, x9, #(1 << 2)       // C  = 0  (D-cache off)
    bic     x9, x9, #(1 << 12)      // I  = 0  (I-cache off)
    msr     sctlr_el1, x9
    isb

    // ---- flush translation + instruction caches ----
    tlbi    vmalle1
    ic      iallu
    dsb     sy
    isb

    // ---- copy len bytes  src(x0) -> dst(x1)  (MMU off => PA == VA) ----
1:  cbz     x2, 2f
    ldrb    w10, [x0], #1
    strb    w10, [x1], #1
    sub     x2, x2, #1
    b       1b

2:  dsb     sy
    ic      iallu                   // new code is in RAM; drop stale I-lines
    dsb     sy
    isb
    br      x3                      // jump into the freshly-loaded kernel
chainload_stub_end:

// ---------------------------------------------------------------------
// chainload_park — where a secondary core goes to die.
//
// Cores 1-3 idle in smp_worker_loop() (system/smp.c), which lives in .text
// at 0x80000+ — exactly the region the stub above overwrites.  Left running,
// they execute instructions that are being rewritten under them, and their
// page tables (in .bss) are overwritten too.  Before the copy, core 0 hands
// each secondary this address; the core turns its own MMU and caches off so
// it needs no translation, acknowledges, and spins in a low-power loop until
// the new kernel starts it again.
//
// Relocated by the C wrapper to a safe high address, clear of both 0x80000
// and the staging buffer.  No stack use.
//
//   x0 = address of this core's ack byte (also outside the copy region)
// ---------------------------------------------------------------------
.global chainload_park
.global chainload_park_end
chainload_park:
    mrs     x9, sctlr_el1
    bic     x9, x9, #(1 << 0)       // M = 0  (MMU off -> PA == VA, no tables)
    bic     x9, x9, #(1 << 2)       // C = 0
    bic     x9, x9, #(1 << 12)      // I = 0
    msr     sctlr_el1, x9
    isb
    tlbi    vmalle1
    ic      iallu
    dsb     sy
    isb

    mov     w10, #1
    strb    w10, [x0]               // "I am parked" — core 0 polls this
    dsb     sy

1:  wfe
    b       1b
chainload_park_end:
