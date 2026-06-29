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

    /* Enable the MMU (SCTLR.M) + the INSTRUCTION cache (SCTLR.I).  The DATA
     * cache (SCTLR.C) stays OFF so the lock-free worker-pool stays coherent
     * (every data access goes straight to RAM) and DMA buffers (net/SD/WiFi/fb)
     * need no maintenance.  Enabling the I-cache is safe — instructions are not
     * DMA'd — and is the bulk of the speedup for the instruction-fetch-bound
     * compute loops (uncached I-fetch from DRAM was the main cost).  Self-
     * modifying paths (the cc JIT, chainload/kexec) invalidate the I-cache
     * themselves.  Invalidate I-cache before turning it on. */
    __asm__ volatile ("ic iallu\n dsb sy\n isb\n");
    unsigned long sctlr;
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 1UL | (1UL << 12);            /* M = 1, I = 1 (D-cache C stays 0) */
    __asm__ volatile ("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile ("isb");
}

/* Secondary-core MMU bring-up — called from smp_secondary_entry()
 * (system/smp.c) on cores 1-3.  Reuses the page tables core 0 built in
 * mmu_init(): just point this core's MAIR/TCR/TTBR0 at the same l0 root and
 * enable the MMU (M=1).  Caches stay OFF exactly like core 0 (SCTLR.C/I left
 * untouched) — that is what makes the lock-free worker-pool coherent: every
 * access goes straight to RAM.  Does NOT rebuild the tables. */
void mmu_enable_secondary(void)
{
    /* EL1 FP/SIMD access, same as mmu_init — avm/cc do double arithmetic that
     * may run on a worker core via smp_parallel_sum. */
    __asm__ volatile ("msr cpacr_el1, %0\n isb\n" :: "r"(3UL << 20) : "memory");

    unsigned long mair = (0x00UL << 0) | (0xFFUL << 8);
    unsigned long tcr =
          (16UL <<  0)      /* T0SZ = 16 -> 48-bit VA                 */
        | (1UL  <<  8)      /* IRGN0 = WBWA                           */
        | (1UL  << 10)      /* ORGN0 = WBWA                           */
        | (3UL  << 12)      /* SH0   = inner shareable                */
        | (0UL  << 14)      /* TG0   = 4KB                            */
        | (1UL  << 23)      /* EPD1  = 1 (no TTBR1 walks)             */
        | (5UL  << 32);     /* IPS   = 48-bit                         */
    __asm__ volatile ("msr mair_el1, %0"  :: "r"(mair));
    __asm__ volatile ("msr tcr_el1,  %0"  :: "r"(tcr));
    __asm__ volatile ("msr ttbr0_el1, %0" :: "r"((unsigned long)l0));
    __asm__ volatile ("dsb sy\n tlbi vmalle1\n dsb sy\n isb\n");

    __asm__ volatile ("ic iallu\n dsb sy\n isb\n");
    unsigned long sctlr;
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= 1UL | (1UL << 12);            /* M = 1, I = 1 (D-cache C stays 0) */
    __asm__ volatile ("msr sctlr_el1, %0" :: "r"(sctlr));
    __asm__ volatile ("isb");
}

/* ====================================================================
 *  Demand-paged virtual memory.
 *
 *  A virtual window with NO physical backing until first touch.  We pick a
 *  high VA region that is neither RAM (Pi 5: <= 16 GiB) nor a real device
 *  (Pi 5 devices live at >= 64 GiB), splice an L2 table whose L3 leaves are
 *  all INVALID into the L1 entry that the identity map left as a (do-nothing)
 *  device block.  Any access in [VMD_BASE, VMD_END) then translation-faults;
 *  the sync exception trampoline (exception_vectors.S -> sync_dispatch_c)
 *  calls vm_fault(), which grabs a physical frame from a pool, installs the
 *  L3 page-table entry, flushes the TLB, and returns so the faulting
 *  instruction re-executes against the now-valid mapping.
 *
 *  Caches are off here just like the rest of the kernel, so no cache
 *  maintenance of the freshly-zeroed frame is needed — the CPU reads RAM
 *  directly.  A single dsb+tlbi for the new VA is enough.
 * ==================================================================== */
#define PAGE        4096UL
#define NENT        512
#define TWO_MB      0x200000UL
#define DESC_PAGE   0x3UL                 /* valid L3 page descriptor          */

#define VMD_BASE    0x800000000UL         /* 32 GiB — above RAM, below devices */
#define VMD_L1_IDX  ((int)(VMD_BASE >> 30))   /* = 32 (into l1_lo)             */
#define VMD_2MB     2                     /* 2 x 2 MiB = 4 MiB virtual window  */
#define VMD_END     (VMD_BASE + (unsigned long)VMD_2MB * TWO_MB)
#define VMD_POOL_PAGES 512                /* physical frames available on demand */

static unsigned long  l2_vmd[NENT]                __attribute__((aligned(4096)));
static unsigned long  l3_vmd[VMD_2MB][NENT]       __attribute__((aligned(4096)));
static unsigned char  vmd_pool[VMD_POOL_PAGES][PAGE] __attribute__((aligned(4096)));
static int            vmd_pool_next;
volatile unsigned long g_vm_faults, g_vm_mapped, g_vm_oom;

/* Build the page tables for the demand window with every leaf INVALID, then
 * splice the L2 table into l1_lo[VMD_L1_IDX].  After this, any access in
 * [VMD_BASE, VMD_END) translation-faults until vm_fault() maps it. */
void vm_demand_init(void)
{
    for (int i = 0; i < NENT; i++) l2_vmd[i] = 0;
    for (int t = 0; t < VMD_2MB; t++) {
        for (int p = 0; p < NENT; p++) l3_vmd[t][p] = 0;       /* invalid */
        l2_vmd[t] = (unsigned long)&l3_vmd[t][0] | DESC_TABLE;
    }
    l1_lo[VMD_L1_IDX] = (unsigned long)l2_vmd | DESC_TABLE;     /* was a device block */
    __asm__ volatile ("dsb sy\n tlbi vmalle1\n dsb sy\n isb\n" ::: "memory");
}

int vm_demand_region(unsigned long *base, unsigned long *size)
{ if (base) *base = VMD_BASE; if (size) *size = VMD_END - VMD_BASE; return VMD_POOL_PAGES; }

unsigned long vm_fault_count(void)   { return g_vm_faults; }
unsigned long vm_mapped_count(void)  { return g_vm_mapped; }
unsigned long vm_oom_count(void)     { return g_vm_oom; }
unsigned long vm_pool_pages(void)    { return VMD_POOL_PAGES; }
unsigned long vm_pool_used(void)     { return (unsigned long)vmd_pool_next; }

/* Page-fault handler (called from sync_dispatch_c on a translation abort).
 * Returns 1 if `va` is in the demand window and is now mapped (retry the
 * instruction), 0 otherwise (not ours -> fatal fault path). */
int vm_fault(unsigned long va)
{
    if (va < VMD_BASE || va >= VMD_END) return 0;
    unsigned long off = va - VMD_BASE;
    int l2i = (int)(off >> 21);
    int l3i = (int)((off >> 12) & 0x1FF);
    if (l2i < 0 || l2i >= VMD_2MB) return 0;
    unsigned long *pte = &l3_vmd[l2i][l3i];
    if (*pte & 1UL) return 1;                          /* already present (spurious) */
    if (vmd_pool_next >= VMD_POOL_PAGES) { g_vm_oom++; return 0; }   /* out of frames */
    unsigned long pa = (unsigned long)&vmd_pool[vmd_pool_next++][0];
    for (int i = 0; i < (int)(PAGE / 8); i++)
        ((volatile unsigned long *)pa)[i] = 0;         /* zero-fill the fresh frame */
    *pte = (pa & ~(PAGE - 1)) | ATTR_NORMAL | DESC_AF | DESC_PAGE;   /* Normal RW */
    __asm__ volatile ("dsb ish" ::: "memory");
    __asm__ volatile ("tlbi vaae1is, %0" :: "r"(va >> 12) : "memory");
    __asm__ volatile ("dsb ish\n isb\n" ::: "memory");
    g_vm_faults++; g_vm_mapped++;
    return 1;
}
