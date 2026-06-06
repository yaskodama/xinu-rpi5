// device/genet/rp1pcie.c — BCM2712 PCIe root-complex bring-up for the RP1.
//
// The firmware brings PCIe up to boot (USB-MSD via the RP1) but resets it
// before OS handoff, so our kernel sees 0xdeaddead everywhere in the RP1
// window.  This re-initialises the BCM2712 PCIe RC and trains the link to the
// RP1 so 0x1F00xxxxxx (clocks, GEM Ethernet, ...) becomes reachable.
//
// Faithfully ported from circle-ref/lib/bcmpciehostbridge.cpp (pcie_setup +
// helpers), itself from the Linux brcmstb PCIe driver.  Milestone 1: get
// pcie_link_up() true; then the RP1 clock controller / GEM stop reading
// 0xdeaddead.  MMU is off, so every address here is < 2^40 (0x10_xxxx RC /
// 0x1F_xxxx RP1 window) and reachable.

#include "uart.h"

#ifdef RP1_ETH_BASE

typedef unsigned int       u32;
typedef unsigned long      u64;

#define PCIE_BASE    0x1000120000UL   /* ARM_PCIE_HOST_BASE (onboard / RP1) */
#define RESET_BASE   0x1001504318UL   /* brcmstb SW_INIT reset controller   */
#define RESCAL_BASE  0x1000119500UL   /* SATA/PCIe rescal block             */

#define R(a)    (*(volatile u32 *)(u64)(a))
#define P(off)  R(PCIE_BASE + (off))

/* ---- PCIe RC register offsets (from circle bcmpciehostbridge.cpp) ---- */
#define MISC_CTRL                  0x4008
#define MISC_CPU_2_PCIE_WIN0_LO    0x400c
#define MISC_CPU_2_PCIE_WIN0_HI    0x4010
#define MISC_RC_BAR1_LO            0x402c
#define MISC_RC_BAR2_LO            0x4034
#define MISC_RC_BAR2_HI            0x4038
#define MISC_RC_BAR3_LO            0x403c
#define MISC_RC_CFG_RETRY_TIMEOUT  0x405c
#define MISC_PCIE_CTRL             0x4064
#define MISC_PCIE_STATUS           0x4068
#define MISC_REVISION              0x406c
#define MISC_CPU_2_PCIE_WIN0_BLIM  0x4070
#define MISC_CPU_2_PCIE_WIN0_BHI   0x4080
#define MISC_CPU_2_PCIE_WIN0_LHI   0x4084
#define MISC_CTRL_1                0x40a0
#define MISC_UBUS_CTRL             0x40a4
#define MISC_UBUS_TIMEOUT          0x40a8
#define MISC_UBUS_BAR2_REMAP       0x40b4
#define MISC_AXI_INTF_CTRL         0x416c
#define MISC_AXI_READ_ERROR_DATA   0x4170
#define MISC_HARD_DEBUG            0x4204
#define RC_CFG_VENDOR_SPEC_REG1    0x0188
#define RC_CFG_PRIV1_ID_VAL3       0x043c
#define RC_CFG_PRIV1_LINK_CAP      0x04dc
#define RC_PL_PHY_CTL_15           0x184c
#define RC_DL_MDIO_ADDR            0x1100
#define RC_DL_MDIO_WR_DATA         0x1104
#define RC_DL_MDIO_RD_DATA         0x1108
#define CAP_REGS                   0x00ac          /* PCIe capability regs   */
#define PCI_EXP_LNKCAP             0x0c
#define PCI_EXP_LNKCTL2            0x30

#define HARD_DEBUG_SERDES_IDDQ     0x08000000
#define PCIE_CTRL_PERSTB           0x4             /* bit 2                  */
#define STATUS_DL_ACTIVE           0x20            /* bit 5                  */
#define STATUS_PHYLINKUP           0x10            /* bit 4                  */

/* ---- accurate delays from the always-running generic-timer counter ---- */
static u64 cntpct(void){ u64 v; __asm__ volatile("mrs %0, cntpct_el0":"=r"(v)); return v; }
static u64 cntfrq(void){ u64 v; __asm__ volatile("mrs %0, cntfrq_el0":"=r"(v)); return v; }
static void udelay(u64 us)
{
    u64 f = cntfrq(); if (!f) { for (volatile u64 i=0;i<us*50;i++){} return; }
    u64 end = cntpct() + (f/1000000UL)*us + 1;
    while (cntpct() < end) { }
}
static void msdelay(u64 ms){ udelay(ms*1000UL); }

static void puthex(u32 v){ for(int i=7;i>=0;i--){unsigned n=(v>>(i*4))&0xF;uart_putc((char)(n<10?'0'+n:'a'+n-10));} }

/* ---- reset controller + rescal ---- */
#define SW_INIT_SET 0x00
#define SW_INIT_CLR 0x04
#define SW_INIT_BANK_SIZE 0x18
static void reset_assert(int id)   { R(RESET_BASE + (id>>5)*SW_INIT_BANK_SIZE + SW_INIT_SET) = 1u<<(id&0x1f); }
static void reset_deassert(int id) { R(RESET_BASE + (id>>5)*SW_INIT_BANK_SIZE + SW_INIT_CLR) = 1u<<(id&0x1f); udelay(200); }

static int rescal_deassert(void)
{
    R(RESCAL_BASE + 0x0) |= 1u;                 /* START bit0                */
    for (volatile u64 t=0; t<2000000; t++)
        if (R(RESCAL_BASE + 0x8) & 1u) break;   /* wait STATUS bit0          */
    R(RESCAL_BASE + 0x0) &= ~1u;
    return (R(RESCAL_BASE + 0x8) & 1u) ? 0 : -1;
}

/* ---- PCIe internal MDIO (for the SerDes PLL munge) ---- */
static u32 mdio_pkt(int port, int regad, int cmd)
{
    return ((u32)(port & 0xf) << 16) | ((u32)(cmd & 0xfff) << 20) | ((u32)regad & 0xffff);
}
static void pcie_mdio_write(int port, int regad, u32 wrdata)
{
    P(RC_DL_MDIO_ADDR) = mdio_pkt(port, regad, 0 /*write*/);
    (void)P(RC_DL_MDIO_ADDR);
    P(RC_DL_MDIO_WR_DATA) = 0x80000000u | (wrdata & 0xffff);
    for (int t=0; t<10; t++) { if (!(P(RC_DL_MDIO_WR_DATA) & 0x80000000u)) break; udelay(10); }
}
static void pcie_munge_pll(void)   /* allow the 54 MHz xosc refclk source */
{
    static const u32 regs[] = { 0x16,0x17,0x18,0x19,0x1b,0x1c,0x1e };
    static const u32 data[] = { 0x50b9,0xbda1,0x0094,0x97b4,0x5030,0x5030,0x0007 };
    pcie_mdio_write(0, 0x1f /*SET_ADDR_OFFSET*/, 0x1600);
    for (unsigned i=0;i<7;i++) pcie_mdio_write(0, regs[i], data[i]);
    udelay(200);
}

static void set_gen(int gen)       /* clamp link speed to GEN2 */
{
    u32 lnkcap = P(CAP_REGS + PCI_EXP_LNKCAP);
    lnkcap = (lnkcap & ~0xfu) | gen;
    P(CAP_REGS + PCI_EXP_LNKCAP) = lnkcap;
    /* LNKCTL2 is 16-bit; low nibble = target speed */
    u32 c2 = P(CAP_REGS + PCI_EXP_LNKCTL2);
    c2 = (c2 & ~0xfu) | gen;
    P(CAP_REGS + PCI_EXP_LNKCTL2) = c2;
}

static int ilog2u(u64 v){ int n=-1; while(v){v>>=1;n++;} return n; }
static int encode_ibar(u64 size)
{
    int l = ilog2u(size);
    if (l>=12 && l<=15) return (l-12)+0x1c;
    if (l>=16 && l<=37) return l-15;
    return 0;
}

static void set_outbound_win(u64 cpu_addr, u64 pcie_addr, u64 size)
{
    P(MISC_CPU_2_PCIE_WIN0_LO) = (u32)pcie_addr;
    P(MISC_CPU_2_PCIE_WIN0_HI) = (u32)(pcie_addr >> 32);
    u64 base_mb  = cpu_addr >> 20;
    u64 limit_mb = (cpu_addr + size - 1) >> 20;
    /* BASE_LIMIT: BASE bits 4..15 (0xfff0), LIMIT bits 20..31 (0xfff00000) */
    u32 bl = P(MISC_CPU_2_PCIE_WIN0_BLIM);
    bl &= ~0xfff0u;     bl |= ((u32)base_mb  << 4)  & 0xfff0u;
    bl &= ~0xfff00000u; bl |= ((u32)limit_mb << 20) & 0xfff00000u;
    P(MISC_CPU_2_PCIE_WIN0_BLIM) = bl;
    /* high bits (>> 12 of the MB value) into BASE_HI / LIMIT_HI (8 bits) */
    P(MISC_CPU_2_PCIE_WIN0_BHI) = (u32)(base_mb  >> 12) & 0xff;
    P(MISC_CPU_2_PCIE_WIN0_LHI) = (u32)(limit_mb >> 12) & 0xff;
}

static int pcie_link_up(void)
{
    u32 s = P(MISC_PCIE_STATUS);
    return ((s & STATUS_DL_ACTIVE) && (s & STATUS_PHYLINKUP)) ? 1 : 0;
}

/* ---- PCIe enumeration: program the RC bridge + the RP1 endpoint so the
 * RP1's BAR claims PCIe addr 0 and memory decode is on. ---- */
#define EXT_CFG_INDEX 0x9000
#define EXT_CFG_DATA  0x8000
#define CMD_MEM_BM    (0x2u|0x4u|0x40u|0x100u)   /* MEMORY|MASTER|PARITY|SERR */

#define W8(a,v)   (*(volatile unsigned char  *)(u64)(a) = (unsigned char)(v))
#define W16(a,v)  (*(volatile unsigned short *)(u64)(a) = (unsigned short)(v))
#define W32(a,v)  (*(volatile u32            *)(u64)(a) = (u32)(v))
#define RD32(a)   (*(volatile u32 *)(u64)(a))

static u64 ep(int reg)   /* bus1/slot0/func0 config window */
{
    P(EXT_CFG_INDEX) = (1u << 20);          /* cfg_index(bus1,devfn0,0) */
    return PCIE_BASE + EXT_CFG_DATA + reg;
}

static void pcie_enumerate(void)
{
    /* --- the RC bridge (bus 0, its own config @ PCIE_BASE+reg) --- */
    W8 (PCIE_BASE + 0x19, 1);               /* secondary bus   = 1 */
    W8 (PCIE_BASE + 0x1a, 1);               /* subordinate bus = 1 */
    W16(PCIE_BASE + 0x20, 0);               /* memory base  (pcie 0 >> 16) */
    W16(PCIE_BASE + 0x22, 0);               /* memory limit */
    W16(PCIE_BASE + 0x3e, 1);               /* bridge control: parity */
    W16(PCIE_BASE + 0x04, CMD_MEM_BM);      /* bridge command */

    /* --- the RP1 endpoint (bus 1) --- */
    u32 viddid = RD32(ep(0x00));
    uart_puts("pcie: EP bus1 VID/DID = 0x"); puthex(viddid); uart_puts("\n");

    W8 (ep(0x0c), 16);                      /* cache line size 64/4 */
    W32(ep(0x10), 0x0u | 0x4u);             /* BAR0 = pcie 0, 64-bit mem */
    W32(ep(0x14), 0x0u);                    /* BAR0 high */
    W32(ep(0x18), 0x00400000u);             /* BAR2 = RP1 SRAM pcie 0x400000 */
    W32(ep(0x1c), 0x0u);                    /* BAR2 high */
    W16(ep(0x04), CMD_MEM_BM);              /* endpoint command: memory enable */
}

/* Returns 0 if the link to the RP1 came up. */
int rp1pcie_init(void)
{
    /* ALWAYS do the full bring-up.  (An earlier "skip if link already up"
     * idempotency broke networking: this firmware leaves the link UP at OS
     * handoff, so the skip also skipped the RC_BAR2 inbound-window setup that
     * the GEM's +0x1000000000 DMA offset depends on -> RX/TX dead.  The skip
     * also never made networking survive a chainload, so it's pure downside.) */
    uart_puts("pcie: RC @0x1000120000 bring-up ...\n");

    if (rescal_deassert() != 0)
        uart_puts("pcie: WARN rescal did not complete\n");

    /* reset the bridge (sw_init id 44 = onboard), then release */
    reset_assert(44);
    udelay(200);
    reset_deassert(44);

    /* take the SerDes out of IDDQ, let it stabilise */
    P(MISC_HARD_DEBUG) &= ~(u32)HARD_DEBUG_SERDES_IDDQ;
    udelay(200);

    uart_puts("pcie: HW rev=0x"); puthex(P(MISC_REVISION) & 0xffff); uart_puts("\n");

    pcie_munge_pll();

    /* L1SS errata: PM clock period ~18.52ns */
    { u32 t = P(RC_PL_PHY_CTL_15); t &= ~0xffu; t |= 0x12; P(RC_PL_PHY_CTL_15) = t; }

    /* MISC_CTRL: SCB_ACCESS_EN | CFG_READ_UR_MODE | BURST_256 | RCB_MPS_MODE */
    { u32 t = P(MISC_CTRL);
      t |= 0x1000;                 /* SCB_ACCESS_EN  */
      t |= 0x2000;                 /* CFG_READ_UR    */
      t &= ~0x300000u; t |= (1u<<20);  /* MAX_BURST_SIZE = 256 */
      t |= 0x400;                  /* RCB_MPS_MODE   */
      /* SCB0_SIZE for a 4GB inbound view: ilog2(4G)-15 = 17 (0x11) */
      t &= ~0xf8000000u; t |= ((u32)17 << 27) & 0xf8000000u;
      P(MISC_CTRL) = t; }

    /* inbound view (RC_BAR2): Pi 5 maps host RAM at PCIe addr 0x10_00000000
     * (dtb dma-ranges: host 0 -> PCIe 0x1000000000).  So a device DMAs to
     * host P using PCIe address P + 0x1000000000. */
    P(MISC_RC_BAR2_LO) = (u32)(0x1000000000UL & 0xffffffffu) | (u32)encode_ibar(0x100000000UL);
    P(MISC_RC_BAR2_HI) = (u32)(0x1000000000UL >> 32);     /* = 0x10 */
    P(MISC_UBUS_BAR2_REMAP) |= 1u;          /* ACCESS_ENABLE (remap host base 0) */

    /* suppress AXI error responses; return 1s on read failure */
    P(MISC_UBUS_CTRL) |= (1u<<13) | (1u<<19);
    P(MISC_AXI_READ_ERROR_DATA) = 0xffffffffu;
    P(MISC_UBUS_TIMEOUT)          = 0x0B2D0000u;
    P(MISC_RC_CFG_RETRY_TIMEOUT)  = 0x0ABA0000u;

    /* disable RC_BAR1 / RC_BAR3 (SIZE field = 0) */
    P(MISC_RC_BAR1_LO) &= ~0x1fu;
    P(MISC_RC_BAR3_LO) &= ~0x1fu;

    /* advertise ASPM L0s+L1 */
    { u32 t = P(RC_CFG_PRIV1_LINK_CAP); t &= ~0xc00u; t |= (3u<<10) & 0xc00u; P(RC_CFG_PRIV1_LINK_CAP)=t; }

    set_gen(2);

    /* show RC as a PCIe-PCIe bridge (class 0x060400), not EP */
    { u32 t = P(RC_CFG_PRIV1_ID_VAL3); t &= ~0xffffffu; t |= 0x060400u; P(RC_CFG_PRIV1_ID_VAL3)=t; }

    /* outbound window: CPU 0x1F00000000 -> PCIe 0x0 (RP1) */
    set_outbound_win(0x1F00000000UL, 0x0UL, 0xFFFFFFFCUL);

    /* little-endian BAR2 */
    P(RC_CFG_VENDOR_SPEC_REG1) &= ~0xcu;

    /* deassert PERST (PERSTB bit; assert value is 0, so write 1) */
    { u32 t = P(MISC_PCIE_CTRL); t |= PCIE_CTRL_PERSTB; P(MISC_PCIE_CTRL)=t; }

    msdelay(100);                            /* CEM: 100ms after PERST# */

    /* poll for link up, up to ~1s */
    for (int i=0; i<200 && !pcie_link_up(); i++) msdelay(5);

    u32 st = P(MISC_PCIE_STATUS);
    uart_puts("pcie: STATUS=0x"); puthex(st);
    uart_puts(pcie_link_up() ? "  LINK UP\n" : "  link down\n");
    if (!pcie_link_up()) return -1;

    /* link is up — enumerate so the RP1's BAR claims PCIe addr 0 */
    pcie_enumerate();

    /* now the RP1 window should be live (not 0xdeaddead / 0xffffffff) */
    uart_puts("pcie: RP1 clocks@0x1F00018000 = 0x");
    puthex(R(0x1F00018000UL)); uart_puts("\n");

    return 0;
}

#endif /* RP1_ETH_BASE */
