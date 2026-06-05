// device/genet/rp1eth.c — Raspberry Pi 5 RP1 Gigabit Ethernet (Cadence GEM).
//
// The Pi 5's Ethernet MAC is NOT the BCM2711 GENET (that lives at a fixed
// SoC address on the Pi 4 — see genet.c).  It is a Cadence GEM / MACB
// ("raspberrypi,rp1-gem","cdns,macb") inside the RP1 I/O chip, reached over
// PCIe.  The firmware maps the RP1 register window at 0x1F00000000 on the
// BCM2712 side; the GEM is ethernet@100000 -> 0x1F00100000 (size 0x4000).
// That address is < 2^40 so it is reachable even with the MMU off (unlike
// the 41-bit GIC).  PCIe must be kept up: config.txt needs pciex4_reset=0.
//
// MILESTONE 1 (this file, for now): prove we can reach the controller over
// PCIe — read the module ID, the firmware-programmed MAC, and the PHY ID
// over MDIO.  The DMA descriptor rings + TX/RX (modelled on genet.c's
// interface and U-Boot drivers/net/macb.c) land next once this probe
// confirms the path is alive.

#include "uart.h"

#ifdef RP1_ETH_BASE

/* Cadence GEM register file (offsets from U-Boot drivers/net/macb.h). */
#define GEM_NCR     0x000   /* Network Control                         */
#define GEM_NCFGR   0x004   /* Network Config                          */
#define GEM_NSR     0x008   /* Network Status (bit2 = MDIO idle)       */
#define GEM_MAN     0x034   /* PHY Maintenance (MDIO)                   */
#define GEM_SA1B    0x088   /* Specific-address 1 bottom (MAC[3:0])    */
#define GEM_SA1T    0x08c   /* Specific-address 1 top    (MAC[5:4])    */
#define GEM_MID     0x0fc   /* Module ID (IDNUM[27:16] >= 2 => a GEM)  */

#define NCR_MPE     (1u << 4)   /* management port enable              */
#define NSR_IDLE    (1u << 2)   /* MDIO transfer complete              */

#define E(off)  (*(volatile unsigned int *)((unsigned long)RP1_ETH_BASE + (off)))

/* RP1 clock controller (clocks@18000 -> ARM 0x1F00018000).  Each clock has a
 * CTRL register; bit 11 is the enable gate (from rpi linux clk-rp1.c).  The
 * GEM's pclk/hclk is CLK_SYS (already on), but the ETH (tx, id16) and ETH_TSU
 * (id29) gates are off after firmware handoff, so the block reads 0xdeaddead
 * (RP1's poison for an un-clocked peripheral) until we enable them. */
#define RP1_CLOCKS_BASE   0x1F00018000UL
#define C(off)  (*(volatile unsigned int *)(RP1_CLOCKS_BASE + (off)))
#define CLK_ETH_CTRL      0x064
#define CLK_ETH_TSU_CTRL  0x134
#define CLK_CTRL_ENABLE   (1u << 11)

static void put_hex32(unsigned int v)
{
    for (int i = 7; i >= 0; i--) {
        unsigned int n = (v >> (i * 4)) & 0xF;
        uart_putc((char)(n < 10 ? '0' + n : 'a' + n - 10));
    }
}

static void rp1_clk_enable_eth(void)
{
    uart_puts("rp1eth: CLK_ETH_CTRL was 0x");     put_hex32(C(CLK_ETH_CTRL));
    uart_puts(" TSU 0x");                          put_hex32(C(CLK_ETH_TSU_CTRL));
    uart_puts("\n");
    C(CLK_ETH_CTRL)     |= CLK_CTRL_ENABLE;
    C(CLK_ETH_TSU_CTRL) |= CLK_CTRL_ENABLE;
    for (volatile unsigned long i = 0; i < 200000u; i++) { }   /* settle */
}

/* Clause-22 MDIO read.  MAN = SOF(01) RW(10=read) PHYA REGA CODE(10). */
unsigned int rp1eth_mdio_read(int phy, int reg)
{
    E(GEM_MAN) = (1u << 30) | (2u << 28)
               | ((unsigned)(phy & 0x1f) << 23)
               | ((unsigned)(reg & 0x1f) << 18)
               | (2u << 16);
    for (volatile unsigned long t = 0; t < 1000000u; t++)
        if (E(GEM_NSR) & NSR_IDLE) break;
    return E(GEM_MAN) & 0xffff;
}

void rp1eth_get_mac(unsigned char mac[6])
{
    unsigned int lo = E(GEM_SA1B), hi = E(GEM_SA1T);
    mac[0] = lo & 0xff;  mac[1] = (lo >> 8) & 0xff;
    mac[2] = (lo >> 16) & 0xff; mac[3] = (lo >> 24) & 0xff;
    mac[4] = hi & 0xff;  mac[5] = (hi >> 8) & 0xff;
}

unsigned int rp1eth_module_id(void) { return E(GEM_MID); }

/* Returns 0 if a Cadence GEM answered at RP1_ETH_BASE (PCIe path alive). */
int rp1eth_probe(void)
{
    /* RP1: ungate the Ethernet clocks first, or the GEM reads 0xdeaddead. */
    rp1_clk_enable_eth();

    /* Enable the MDIO management port so PHY reads work. */
    E(GEM_NCR) |= NCR_MPE;

    unsigned int mid = E(GEM_MID);
    unsigned int idnum = (mid >> 16) & 0xfff;     /* IDNUM field           */
    int alive = (mid != 0xdeaddeadu && mid != 0xffffffffu && idnum >= 2);

    uart_puts("rp1eth: GEM @0x1F00100000 MID=0x"); put_hex32(mid);
    if (mid == 0xdeaddeadu)      uart_puts("  (RP1 poison — block still un-clocked/in-reset)\n");
    else if (mid == 0xffffffffu) uart_puts("  (no response — PCIe/RP1 down)\n");
    else if (idnum >= 2)         uart_puts("  (Cadence GEM alive!)\n");
    else                         uart_puts("  (unexpected)\n");

    unsigned char mac[6];
    rp1eth_get_mac(mac);
    uart_puts("rp1eth: MAC = ");
    for (int i = 0; i < 6; i++) {
        unsigned int b = mac[i];
        uart_putc("0123456789abcdef"[(b >> 4) & 0xf]);
        uart_putc("0123456789abcdef"[b & 0xf]);
        if (i < 5) uart_putc(':');
    }
    uart_puts("\n");

    unsigned int id1 = rp1eth_mdio_read(1, 2);    /* PHY ID reg 2 */
    unsigned int id2 = rp1eth_mdio_read(1, 3);    /* PHY ID reg 3 */
    uart_puts("rp1eth: PHY@1 id = 0x"); put_hex32((id1 << 16) | id2);
    uart_puts((id1 == 0xffff && id2 == 0xffff) ? "  (no MDIO response)\n" : "\n");

    return alive ? 0 : -1;
}

#endif /* RP1_ETH_BASE */
