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

static void msdelay(unsigned ms)
{
    unsigned long f, end, now;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
    end = now + (f / 1000u) * ms + 1;
    do { __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now)); } while (now < end);
}

/* RP1 GPIO output, the proper 3-layer way (from circle gpiopin2712.cpp):
 * PADS (clear OD = enable driver), CTRL FUNCSEL=5 (sys_rio), RIO OE/OUT with
 * the atomic SET(+0x2000)/CLR(+0x3000) aliases.  ETH_RST_N = GPIO 32 =
 * bank 1, pin 4, active-low — pulse low then release high. */
#define RP1_GPIO_IO    0x1F000D0000UL
#define RP1_GPIO_RIO   0x1F000E0000UL
#define RP1_GPIO_PADS  0x1F000F0000UL
#define G_CTRL(bk,pn)  (*(volatile unsigned int *)(RP1_GPIO_IO   + (unsigned long)(bk)*0x4000 + (unsigned long)(pn)*8 + 4))
#define G_PAD(bk,pn)   (*(volatile unsigned int *)(RP1_GPIO_PADS + (unsigned long)(bk)*0x4000 + 4 + (unsigned long)(pn)*4))
#define G_RIO(bk,off)  (*(volatile unsigned int *)(RP1_GPIO_RIO  + (unsigned long)(bk)*0x4000 + (off)))
#define RIO_SET 0x2000
#define RIO_CLR 0x3000
static void rp1_phy_reset(void)
{
    const int bk = 1, pn = 4;
    unsigned int pad = G_PAD(bk, pn);
    pad &= ~(1u << 7);                 /* OD=0: enable the output driver  */
    pad |=  (1u << 6);                 /* IE=1                            */
    G_PAD(bk, pn) = pad;
    G_CTRL(bk, pn) = 5u;               /* FUNCSEL = sys_rio (software GPIO) */
    G_RIO(bk, RIO_SET + 4) = (1u << pn);   /* RIO_OE  set  -> output enable */
    G_RIO(bk, RIO_CLR + 0) = (1u << pn);   /* RIO_OUT clr  -> drive LOW     */
    msdelay(10);                       /* assert reset (>5ms)             */
    G_RIO(bk, RIO_SET + 0) = (1u << pn);   /* RIO_OUT set  -> drive HIGH    */
    msdelay(30);                       /* let the PHY come up             */
}

/* Program a locally-administered MAC into the GEM's address-1 filter. */
#define GEM_NCFGR 0x004
static void rp1eth_set_mac(void)
{
    static const unsigned char mac[6] = { 0x02, 0xCA, 0xFE, 0xB0, 0x05, 0x01 };
    E(GEM_SA1B) = mac[0] | (mac[1]<<8) | (mac[2]<<16) | (mac[3]<<24);
    E(GEM_SA1T) = mac[4] | (mac[5]<<8);
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

void rp1eth_mdio_write(int phy, int reg, unsigned int data)
{
    E(GEM_MAN) = (1u << 30) | (1u << 28)          /* SOF, OP=01 (write) */
               | ((unsigned)(phy & 0x1f) << 23)
               | ((unsigned)(reg & 0x1f) << 18)
               | (2u << 16) | (data & 0xffff);
    for (volatile unsigned long t = 0; t < 1000000u; t++)
        if (E(GEM_NSR) & NSR_IDLE) break;
}

void rp1eth_get_mac(unsigned char mac[6])
{
    unsigned int lo = E(GEM_SA1B), hi = E(GEM_SA1T);
    mac[0] = lo & 0xff;  mac[1] = (lo >> 8) & 0xff;
    mac[2] = (lo >> 16) & 0xff; mac[3] = (lo >> 24) & 0xff;
    mac[4] = hi & 0xff;  mac[5] = (hi >> 8) & 0xff;
}

unsigned int rp1eth_module_id(void) { return E(GEM_MID); }

/* ------------------------------------------------------------------ *
 * Cadence GEM DMA driver — descriptor rings + TX/RX.  DMA is coherent *
 * (MMU off => uncached), and RC_BAR2 maps PCIe addr 0 -> host RAM 1:1, *
 * so the GEM's DMA addresses are just our physical (== virtual) addrs.*
 * ------------------------------------------------------------------ */
extern void *getmem(unsigned long nbytes);

#define GEM_NCR     0x000
#define GEM_DMACFG  0x010
#define GEM_RBQP    0x018
#define GEM_TBQP    0x01c
#define GEM_TBQPH   0x4c8
#define GEM_RBQPH   0x4d4
#define DMACFG_ADDR64 (1u<<30)

#define NCR_RE      (1u<<2)
#define NCR_TE      (1u<<3)
#define NCR_MPE_B   (1u<<4)
#define NCR_TSTART  (1u<<9)

#define RXN   32
#define TXN   2
#define RXBUF 2048

/* Pi 5: devices DMA to host phys P via PCIe address P + 0x1000000000 (the RC
 * RC_BAR2 inbound window).  That is > 32 bits, so use the GEM's 64-bit
 * descriptor format (addr-lo, ctrl, addr-hi, reserved) + ADDR64. */
#define DMA_OFFSET  0x1000000000UL
#define DA(p)  ((unsigned long)(p) + DMA_OFFSET)
#define LO(x)  ((unsigned int)((unsigned long)(x) & 0xffffffffu))
#define HI(x)  ((unsigned int)((unsigned long)(x) >> 32))

struct gem_desc { volatile unsigned int addr, ctrl, addrh, resvd; };
static struct gem_desc *g_rxr, *g_txr;
static unsigned char   *g_rxb, *g_txb;
static int g_rxhead, g_txi;
static unsigned long g_rxcnt, g_txcnt;
static unsigned char g_rxlast[16];               /* first 16 bytes of last RX frame */
unsigned long rp1eth_rx_count(void) { return g_rxcnt; }
unsigned long rp1eth_tx_count(void) { return g_txcnt; }
unsigned int  rp1eth_rxlast(int i) { return (i >= 0 && i < 16) ? g_rxlast[i] : 0; }
unsigned int  rp1eth_rsr(void)  { return E(0x020); }            /* RX status */
unsigned int  rp1eth_rxd0(void) { return g_rxr ? g_rxr[0].addr : 0xffffffffu; }

unsigned int rp1eth_phy_bmsr(void) { return rp1eth_mdio_read(1, 1); }   /* BMSR */
int rp1eth_link_up(void) { return (rp1eth_phy_bmsr() & 0x0004) ? 1 : 0; }/* bit2 */

void rp1eth_start(void)
{
    g_rxr = (struct gem_desc *)getmem(RXN * sizeof(struct gem_desc));
    g_txr = (struct gem_desc *)getmem(TXN * sizeof(struct gem_desc));
    g_rxb = (unsigned char *)getmem(RXN * RXBUF);
    g_txb = (unsigned char *)getmem(RXBUF);

    for (int i = 0; i < RXN; i++) {
        unsigned long ba = DA(g_rxb + i*RXBUF);
        g_rxr[i].addr  = (LO(ba) & ~3u) | (i == RXN-1 ? 2u : 0u);  /* WRAP; USED=0 */
        g_rxr[i].addrh = HI(ba);
        g_rxr[i].ctrl  = 0;
    }
    for (int i = 0; i < TXN; i++) {
        g_txr[i].addr  = 0; g_txr[i].addrh = 0;
        g_txr[i].ctrl  = (1u<<31) | (i == TXN-1 ? (1u<<30) : 0u);  /* USED|WRAP */
    }
    g_rxhead = 0; g_txi = 0;

    /* DMACFG: RX buf 2048/64=32 (RXBS<<16), RXBMS=3, TXPBMS, burst 16, ADDR64 */
    E(GEM_DMACFG) = (32u<<16) | (3u<<8) | (1u<<10) | 0x10u | DMACFG_ADDR64;
    E(GEM_RBQP)   = LO(DA(g_rxr));  E(GEM_RBQPH) = HI(DA(g_rxr));
    E(GEM_TBQP)   = LO(DA(g_txr));  E(GEM_TBQPH) = HI(DA(g_txr));

    /* USRIO: select the RGMII interface (GEM_BIT(RGMII) = bit0).  Without this
     * the GEM stays in GMII mode and no frames flow even with the link up. */
    E(0x0c0) = 0x1u;

    /* NCFGR: NO promiscuous — receive only broadcast (ARP) + frames to our MAC,
     * so the slow wm-loop RX drain isn't swamped by all LAN traffic (which was
     * overrunning the ring and dropping the ARP request we need). */
    E(GEM_NCFGR) &= ~(1u << 4);                     /* clear CAF */

    E(GEM_NCR) = NCR_RE | NCR_TE | NCR_MPE_B;       /* enable RX + TX */

    /* Bring the link up: restart autonegotiation, wait for it to complete. */
    rp1eth_mdio_write(1, 0, 0x1200);                /* BMCR: ANEG enable+restart */
    int up = 0;
    for (int s = 0; s < 50; s++) {                  /* up to ~5 s */
        msdelay(100);
        if (rp1eth_mdio_read(1, 1) & 0x0004) { up = 1; break; }   /* BMSR link */
    }

    /* Configure NCFGR speed/duplex from the Broadcom aux status (reg 0x19,
     * bits 10:8 = highest common denominator). */
    unsigned int ncfgr = E(GEM_NCFGR);
    ncfgr &= ~((1u<<0) | (1u<<1) | (1u<<10));       /* clear SPD, FD, GBE */
    unsigned int aux = rp1eth_mdio_read(1, 0x19);
    unsigned int hcd = (aux >> 8) & 0x7;
    /* hcd: 7=1000FD 6=1000HD 5=100FD 3=100HD 2=10FD 1=10HD */
    if (hcd == 7 || hcd == 6) ncfgr |= (1u<<10);                  /* gigabit */
    else if (hcd == 5 || hcd == 3) ncfgr |= (1u<<0);             /* 100 Mbps */
    if (hcd == 7 || hcd == 5 || hcd == 2) ncfgr |= (1u<<1);      /* full duplex */
    E(GEM_NCFGR) = ncfgr;

    uart_puts("rp1eth: aneg "); uart_puts(up ? "UP" : "DOWN");
    uart_puts(" aux=0x"); put_hex32(aux);
    uart_puts(" ncfgr=0x"); put_hex32(ncfgr); uart_puts("\n");
}

int rp1eth_tx_frame(const unsigned char *f, int len)
{
    if (len > RXBUF) len = RXBUF;
    for (int i = 0; i < len; i++) g_txb[i] = f[i];
    int ti = g_txi;
    unsigned long ba = DA(g_txb);
    g_txr[ti].addr  = LO(ba);
    g_txr[ti].addrh = HI(ba);
    g_txr[ti].ctrl  = ((unsigned)len & 0x3fff) | (1u<<15)       /* LAST */
                    | (ti == TXN-1 ? (1u<<30) : 0u);            /* WRAP, USED=0 */
    E(GEM_NCR) |= NCR_TSTART;
    int ok = 0;
    for (int t = 0; t < 1000000; t++)
        if (g_txr[ti].ctrl & (1u<<31)) { ok = 1; break; }       /* USED set = done */
    g_txi = (g_txi + 1) % TXN;
    if (ok) g_txcnt++;
    return ok ? 0 : -1;
}

static void rp1eth_rx_rearm(void)   /* hand the whole RX ring back to HW */
{
    for (int i = 0; i < RXN; i++) {
        unsigned long ba = DA(g_rxb + i*RXBUF);
        g_rxr[i].addr  = (LO(ba) & ~3u) | (i == RXN-1 ? 2u : 0u);
        g_rxr[i].addrh = HI(ba);
        g_rxr[i].ctrl  = 0;
    }
    g_rxhead = 0;
    unsigned int ncr = E(GEM_NCR);
    E(GEM_NCR) = ncr & ~NCR_RE;     /* stop RX, reset the queue pointer, restart */
    E(GEM_RBQP) = LO(DA(g_rxr));  E(GEM_RBQPH) = HI(DA(g_rxr));
    E(0x020) = 0x0fu;               /* clear RSR (REC/BNA/OVR) */
    E(GEM_NCR) = ncr | NCR_RE;
}

int rp1eth_rx_poll(unsigned char **pkt)
{
    if (!(g_rxr[g_rxhead].addr & 1u)) {
        /* Ring empty.  Just clear the sticky RX status (REC/BNA/OVR) so the GEM
         * keeps receiving into the descriptors we've freed — do NOT reset the
         * ring / toggle RE (that was killing RX every time BNA latched). */
        E(0x020) = 0x0fu;
        (void)rp1eth_rx_rearm;
        return 0;
    }
    int len = (int)(g_rxr[g_rxhead].ctrl & 0xfff);
    unsigned char *b = g_rxb + g_rxhead*RXBUF;
    for (int i = 0; i < 16; i++) g_rxlast[i] = b[i];   /* snapshot for HDMI */
    *pkt = b;
    return len;
}

void rp1eth_rx_release(void)
{
    g_rxr[g_rxhead].addr &= ~1u;                                /* give back to HW */
    g_rxhead = (g_rxhead + 1) % RXN;
    g_rxcnt++;
}

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

    /* Bring up the management interface + PHY before reading its ID:
     *  - NCFGR MDC clock divider = pclk/224 (GEM_CLK field, offset 18 = 7)
     *  - program a MAC, then pulse the PHY reset (RP1 GPIO ETH_RST_N). */
    if (alive) {
        unsigned int ncfgr = E(GEM_NCFGR);
        ncfgr &= ~(7u << 18);
        ncfgr |=  (7u << 18);                      /* GEM_CLK_DIV224 */
        E(GEM_NCFGR) = ncfgr;
        rp1eth_set_mac();
        rp1_phy_reset();
    }

    unsigned int id1 = rp1eth_mdio_read(1, 2);    /* PHY ID reg 2 */
    unsigned int id2 = rp1eth_mdio_read(1, 3);    /* PHY ID reg 3 */
    uart_puts("rp1eth: PHY@1 id = 0x"); put_hex32((id1 << 16) | id2);
    uart_puts((id1 == 0xffff && id2 == 0xffff) ? "  (no MDIO response)\n" : "\n");

    if (alive && !(id1 == 0xffff && id2 == 0xffff)) {
        rp1eth_start();
        unsigned int bmsr = rp1eth_phy_bmsr();
        uart_puts("rp1eth: GEM started, link ");
        uart_puts((bmsr & 0x0004) ? "UP" : "down");
        uart_puts(" (BMSR=0x"); put_hex32(bmsr); uart_puts(")\n");

        /* send one broadcast test frame (experimental ethertype 0x88b5) */
        unsigned char tf[64];
        for (int i = 0; i < 6; i++) tf[i] = 0xff;
        tf[6]=0x02; tf[7]=0xca; tf[8]=0xfe; tf[9]=0xb0; tf[10]=0x05; tf[11]=0x01;
        tf[12]=0x88; tf[13]=0xb5;
        for (int i = 14; i < 64; i++) tf[i] = (unsigned char)i;
        int tx = rp1eth_tx_frame(tf, 64);
        uart_puts(tx == 0 ? "rp1eth: TX test frame sent\n" : "rp1eth: TX timeout\n");
        /* diagnose where the DMA stalls */
        uart_puts("rp1eth: TSR=0x");    put_hex32(E(0x014));
        uart_puts(" RSR=0x");           put_hex32(E(0x020));
        uart_puts(" NCR=0x");           put_hex32(E(GEM_NCR));
        uart_puts(" DMACFG=0x");        put_hex32(E(GEM_DMACFG)); uart_puts("\n");
        uart_puts("rp1eth: txdesc[0] addr=0x"); put_hex32(g_txr[0].addr);
        uart_puts(" ctrl=0x");          put_hex32(g_txr[0].ctrl);
        uart_puts("  TBQP=0x");         put_hex32(E(GEM_TBQP)); uart_puts("\n");

        /* briefly poll for any RX */
        int got = 0; unsigned char *p;
        for (int t = 0; t < 4000000 && got < 3; t++) {
            int n = rp1eth_rx_poll(&p);
            if (n > 0) { got++; uart_puts("rp1eth: RX frame len="); put_hex32((unsigned)n); uart_puts("\n"); rp1eth_rx_release(); }
        }
        if (!got) uart_puts("rp1eth: no RX in window\n");
    }

    return alive ? 0 : -1;
}

/* ------------------------------------------------------------------ *
 * Link-layer shim: on the Pi 5 there is no GENET, so route the stack's *
 * genet_* link calls (net_responder / tcp / main loop) to the RP1 GEM. *
 * The GEM is started in rp1eth_probe(), which runs before the stack    *
 * sends its gratuitous ARP / opens the TCP listener.                   *
 * ------------------------------------------------------------------ */
#ifndef GENET_BASE
void genet_init(void) { }                     /* GEM already brought up in probe */
int  genet_tx_frame(const unsigned char *f, int len) { return rp1eth_tx_frame(f, len); }
int  genet_rx_poll(unsigned char **p) { return rp1eth_rx_poll(p); }
void genet_rx_release(void) { rp1eth_rx_release(); }
int  genet_link_up(void) { return rp1eth_link_up(); }
unsigned int genet_phy_bmsr(void) { return rp1eth_phy_bmsr(); }
void genet_get_mac(unsigned char mac[6])
{
    static const unsigned char m[6] = { 0x02, 0xCA, 0xFE, 0xB0, 0x05, 0x01 };
    for (int i = 0; i < 6; i++) mac[i] = m[i];
}
#endif

#endif /* RP1_ETH_BASE */
