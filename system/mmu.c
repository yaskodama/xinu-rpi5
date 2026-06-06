// system/mmu.c — AArch64 MMU bring-up for the Pi 5 (BCM2712 / Cortex-A76).
//
// Why: with the MMU OFF, flat addressing is capped at the CPU's 40-bit PA, so
// the BCM2712 GIC-400 at 0x1007FFF9000 (a 41-bit address) is unreachable and
// the timer/RX interrupts can't be taken (see project memory).  Turning the
// MMU on with a 48-bit translation regime maps that high region in, which is
// the prerequisite for re-enabling the GIC and getting RX interrupts.
//
// Safety: this first cut enables address translation ONLY (SCTLR.M=1) and
// leaves the caches OFF (C=0, I=0).  All RAM is mapped Normal but, with the
// data cache off, every access is effectively non-cacheable — exactly like the
// MMU-off world today — so the framebuffer scan-out and the GEM DMA buffers
// stay coherent and nothing that already works breaks.  (Hazard note: never
// set M together with C/I/Z on the very first enable.)
//
// Identity map (VA == PA), 4KB granule, 48-bit VA via TTBR0:
//   0x0          .. 0x100000000   Normal  (RAM, kernel, heap, framebuffer)
//   0x100000000  .. 0x8000000000  Device  (PCIe RC 0x10_01200000, peripherals
//                                           0x10_7Cxxxxxx, RP1 0x1F_00000000…)
//   0x10000000000.. 0x18000000000 Device  (the GIC-400 lives at 0x1007FFF9000)

#define ENTRIES 512

static unsigned long l0[ENTRIES]   __attribute__((aligned(4096)));
static unsigned long l1_lo[ENTRIES]__attribute__((aligned(4096)));  /* 0 .. 512GB  */
static unsigned long l1_hi[ENTRIES]__attribute__((aligned(4096)));  /* 1TB .. 1.5TB */

#define DESC_BLOCK   0x1UL          /* level-1/2 block                       */
#define DESC_TABLE   0x3UL          /* level-0/1 table pointer               */
#define DESC_AF      (1UL << 10)    /* access flag (else fault)              */
#define ATTR_DEVICE  (0UL << 2)     /* AttrIndx 0 -> MAIR attr0 (Device)     */
#define ATTR_NORMAL  (1UL << 2)     /* AttrIndx 1 -> MAIR attr1 (Normal)     */

#define GIGABYTE     0x40000000UL
#define TB1          0x10000000000UL

void mmu_init(void)
{
    /* Enable EL1 FP/SIMD access (CPACR_EL1.FPEN = 0b11).  The AIPL value_t
     * runtime in cc/cc.c does double arithmetic for float values; without this
     * the first FP instruction it executes traps.  Harmless for integer code. */
    __asm__ volatile ("msr cpacr_el1, %0\n isb\n" :: "r"(3UL << 20) : "memory");

    for (int i = 0; i < ENTRIES; i++) { l0[i] = 0; l1_lo[i] = 0; l1_hi[i] = 0; }

    l0[0] = (unsigned long)l1_lo | DESC_TABLE;   /* VA 0 .. 512GB     */
    l0[2] = (unsigned long)l1_hi | DESC_TABLE;   /* VA 1TB .. 1.5TB   */

    /* low 512GB: first 4GB = RAM/FB (Normal), the rest = Device peripherals */
    for (int i = 0; i < ENTRIES; i++) {
        unsigned long pa = (unsigned long)i * GIGABYTE;
        unsigned long attr = (i < 4) ? ATTR_NORMAL : ATTR_DEVICE;
        l1_lo[i] = pa | attr | DESC_AF | DESC_BLOCK;
    }

    /* 1TB..1.5TB: all Device (covers the GIC-400 at 0x1007FFF9000) */
    for (int i = 0; i < ENTRIES; i++) {
        unsigned long pa = TB1 + (unsigned long)i * GIGABYTE;
        l1_hi[i] = pa | ATTR_DEVICE | DESC_AF | DESC_BLOCK;
    }

    /* MAIR: attr0 = Device-nGnRnE (0x00), attr1 = Normal write-back (0xFF). */
    unsigned long mair = (0x00UL << 0) | (0xFFUL << 8);
    __asm__ volatile ("msr mair_el1, %0" :: "r"(mair));

    /* TCR: T0SZ=16 (48-bit VA), 4KB granule, WBWA/inner-shareable walks,
     * IPS=48-bit, TTBR1 walks disabled (we only use the low half). */
    unsigned long tcr =
          (16UL <<  0)      /* T0SZ = 16 -> 48-bit VA                 */
        | (1UL  <<  8)      /* IRGN0 = WBWA                           */
        | (1UL  << 10)      /* ORGN0 = WBWA                           */
        | (3UL  << 12)      /* SH0   = inner shareable                */
        | (0UL  << 14)      /* TG0   = 4KB                            */
        | (1UL  << 23)      /* EPD1  = 1 (no TTBR1 walks)             */
        | (5UL  << 32);     /* IPS   = 48-bit                         */
    __asm__ volatile ("msr tcr_el1, %0"  :: "r"(tcr));
    __asm__ volatile ("msr ttbr0_el1, %0":: "r"((unsigned long)l0));

    __asm__ volatile ("dsb sy");
    __asm__ volatile ("tlbi vmalle1");
    __asm__ volatile ("dsb sy");
    __asm__ volatile ("isb");

    /* Enable the MMU (SCTLR.M) only — caches stay off on this first enable. */
    unsigned long sctlr;
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 1UL;                          /* M = 1 */
    __asm__ volatile ("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile ("isb");
}
