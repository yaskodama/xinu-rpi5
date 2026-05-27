// device/genet/genet.c — BCM2711 GENET Ethernet driver, phase NET-A.
//
// Block layout (from Linux bcmgenet.h, GENET v5 used on BCM2711):
//
//   GENET_BASE + 0x0000   SYS block      (revision, port ctrl, RBUF flush)
//   GENET_BASE + 0x0040   GR_BRIDGE      (bridge ctrl)
//   GENET_BASE + 0x0080   EXT            (ext-config / PHY isolation)
//   GENET_BASE + 0x0200   INTRL2_0       (interrupt level-2, set 0)
//   GENET_BASE + 0x0240   INTRL2_1       (interrupt level-2, set 1)
//   GENET_BASE + 0x0300   RBUF           (RX buffer ctrl)
//   GENET_BASE + 0x0600   TBUF           (TX buffer ctrl)
//   GENET_BASE + 0x0800   UMAC           (UniMAC core)
//   GENET_BASE + 0x1000   HFB            (hardware filter)
//   GENET_BASE + 0x2040   TDMA ring 0    (TX DMA descriptors)
//   GENET_BASE + 0x4000   RDMA ring 0    (RX DMA descriptors)
//
// NET-A only reads SYS_REV_CTRL and SYS_PORT_CTRL.  A live BCM2711
// reports SYS_REV_CTRL ≈ 0x06000000 (major.minor.patch encoded).

#include "genet.h"
#include "uart.h"
#include "mbox.h"

#ifdef GENET_BASE

#define GENET_REG(off)         (*(volatile unsigned int *)(GENET_BASE + (off)))
#define SYS_REV_CTRL           0x000
#define SYS_PORT_CTRL          0x004
#define SYS_RBUF_FLUSH_CTRL    0x008
#define SYS_TBUF_FLUSH_CTRL    0x00C

/* UMAC (UniMAC core).  Inside Linux this is GENET_BASE + 0x800
 * with internal register offsets numbered from 0.  Useful subset
 * for this phase: */
#define UMAC_BASE              0x800
#define UMAC_HD_BKP_CTRL       (UMAC_BASE + 0x004)
#define UMAC_CMD               (UMAC_BASE + 0x008)
#define UMAC_MAC0              (UMAC_BASE + 0x00C)   /* MAC[5..2] big-end */
#define UMAC_MAC1              (UMAC_BASE + 0x010)   /* MAC[1..0] high half */
#define UMAC_MAX_FRAME_LEN     (UMAC_BASE + 0x014)
#define UMAC_MIB_CTRL          (UMAC_BASE + 0x580)
#define UMAC_MIB_RESET_RX      (1u << 0)
#define UMAC_MIB_RESET_RUNT    (1u << 1)
#define UMAC_MIB_RESET_TX      (1u << 2)

#define CMD_TX_EN              (1u << 0)
#define CMD_RX_EN              (1u << 1)
#define CMD_SPEED_SHIFT        2
#define CMD_SPEED_10           (0u << CMD_SPEED_SHIFT)
#define CMD_SPEED_100          (1u << CMD_SPEED_SHIFT)
#define CMD_SPEED_1000         (2u << CMD_SPEED_SHIFT)
#define CMD_PROMISC            (1u << 4)
#define CMD_PAD_EN             (1u << 5)
#define CMD_CRC_FWD            (1u << 6)
#define CMD_LCL_LOOP_EN        (1u << 15)
#define CMD_SW_RESET           (1u << 13)
#define RBUF_CTRL              0x300

/* MDIO command register inside UMAC: UMAC_BASE + 0x614 = 0xE14 */
#define UMAC_MDIO_CMD          (UMAC_BASE + 0x614)

/* TX DMA block layout (GENET v5 / Pi 4) — from U-Boot bcmgenet.c.
 *
 *   GENET_BASE + 0x4000 ... 0x4BFF: TX descriptor SRAM (256 × 12 B)
 *     descriptor[i] = three 32-bit words at offset 0x4000 + i*12:
 *       +0x00  length_status
 *       +0x04  address_lo
 *       +0x08  address_hi
 *   GENET_BASE + 0x4C00 ... 0x4FFF: per-ring TX control registers
 *     ring base = 0x4C00 + ring * 0x40   (ring 0..15 user, 16 default)
 *   GENET_BASE + 0x5040 ...        : TX top-level (RING_CFG, CTRL, ...)
 *
 *   Default queue 16 ring base = 0x4C00 + 16*0x40 = 0x5000
 *   TX top control            = 0x4C00 + 17*0x40 = 0x5040
 */
#define GENET_TX_OFF                0x4000
#define GENET_RX_OFF                0x2000
#define TX_DESCS                    256
#define RX_DESCS                    256
#define DMA_DESC_SIZE               12       /* 3 × 32-bit words */
#define GENET_TDMA_REG_OFF          (GENET_TX_OFF + TX_DESCS * DMA_DESC_SIZE)
#define GENET_RDMA_REG_OFF          (GENET_RX_OFF + RX_DESCS * DMA_DESC_SIZE)
#define DMA_RING_SIZE               0x40
#define TX_DEFAULT_RING             16
#define TX_RING_BASE                (GENET_TDMA_REG_OFF + TX_DEFAULT_RING * DMA_RING_SIZE)
#define TX_DMA_TOP                  (GENET_TDMA_REG_OFF + 17 * DMA_RING_SIZE)
#define RX_RING_BASE                (GENET_RDMA_REG_OFF + TX_DEFAULT_RING * DMA_RING_SIZE)
#define RX_DMA_TOP                  (GENET_RDMA_REG_OFF + 17 * DMA_RING_SIZE)

#define RX_BUF_LENGTH               2048
/* Per-ring RDMA register offsets (U-Boot bcmgenet) */
#define RDMA_WRITE_PTR              0x00
#define RDMA_PROD_INDEX             0x08
#define RDMA_CONS_INDEX             0x0C
#define RDMA_XON_XOFF_THRESH        0x28
#define RDMA_READ_PTR               0x2C
/* DMA_RING_BUF_SIZE / START_ADDR / END_ADDR / MBUF_DONE_THRESH share TX offsets */

/* Descriptor word offsets (within each 12-byte slot in MMIO) */
#define DMA_DESC_LENGTH_STATUS      0x00
#define DMA_DESC_ADDRESS_LO         0x04
#define DMA_DESC_ADDRESS_HI         0x08

/* Per-ring register offsets — U-Boot bcmgenet layout */
#define TDMA_READ_PTR_LO            0x00
#define TDMA_READ_PTR_HI            0x04   /* unused in U-Boot; kept for symmetry */
#define TDMA_CONS_INDEX             0x08
#define TDMA_PROD_INDEX             0x0C
#define TDMA_RING_BUF_SIZE          0x10
#define TDMA_START_ADDR_LO          0x14
#define TDMA_START_ADDR_HI          0x18
#define TDMA_END_ADDR_LO            0x1C
#define TDMA_END_ADDR_HI            0x20
#define TDMA_MBUF_DONE_THRESH       0x24
#define TDMA_FLOW_PERIOD            0x28
#define TDMA_WRITE_PTR_LO           0x2C
#define TDMA_WRITE_PTR_HI           0x30

/* Top-level DMA control register offsets (relative to TX_DMA_TOP) */
#define TDMA_RING_CFG               0x00     /* bit per ring = enable */
#define TDMA_CTRL                   0x04     /* bit 0 = TDMA enable */
#define TDMA_STATUS                 0x08
#define TDMA_SCB_BURST_SIZE         0x0C

/* EXT block (offset 0x80) — RGMII out-of-band control. */
#define GENET_EXT_OFF               0x80
#define EXT_RGMII_OOB_CTRL          (GENET_EXT_OFF + 0x0C)

/* Speed bits in UMAC_CMD (bits 3:2), updated from PHY AUX_STAT
 * after auto-negotiation completes.  Default 1000 Mbps. */
static unsigned int g_umac_speed_bits = CMD_SPEED_1000;

/* Diagnostic snapshot taken during init — replayed on first RX
 * event so we can see them even if the boot lines scrolled off
 * the shell window. */
static unsigned int g_diag_rgmii_before;
static unsigned int g_diag_rgmii_after;
static unsigned int g_diag_aux_stat;
static unsigned int g_diag_umac_cmd;
#define EXT_OOB_DISABLE             (1u << 5)
#define EXT_RGMII_MODE_EN           (1u << 6)
#define EXT_ID_MODE_DIS             (1u << 16)
#define EXT_RGMII_LINK              (1u << 4)

/* SYS_PORT_CTRL bits */
#define SYS_PORT_MODE_EXT_GPHY      3        /* external 1G PHY */

/* TX descriptor word 0 layout (from U-Boot bcmgenet.h):
 *   bits 31:16 — length (bytes)
 *   bits 15:0  — status flags (positions below)
 */
#define DMA_BUFLENGTH_SHIFT         16
#define DMA_OWN                     0x8000   /* bit 15 (RX uses) */
#define DMA_EOP                     0x4000   /* bit 14 */
#define DMA_SOP                     0x2000   /* bit 13 */
#define DMA_WRAP                    0x1000   /* bit 12 */
#define DMA_TX_APPEND_CRC           0x0040   /* bit 6 */
#define DMA_TX_QTAG_SHIFT           7        /* bits 12:7 (6-bit QTAG) */
#define DMA_TX_DEFAULT_QTAG         0x3F
#define MDIO_START_BUSY        (1u << 29)
#define MDIO_READ_FAIL         (1u << 28)
#define MDIO_RD                (1u << 27)
#define MDIO_WR                (1u << 26)
#define MDIO_PHY_ID_SHIFT      21
#define MDIO_REG_SHIFT         16
#define MDIO_DATA_MASK         0xFFFFu

#define PHY_ID_BCM54213PE      1  /* MDIO address on Pi 4 */

/* IEEE 802.3 MII registers we care about */
#define MII_BMCR               0x00
#define MII_BMSR               0x01
#define MII_PHYSID1            0x02
#define MII_PHYSID2            0x03
#define MII_ANAR               0x04
#define MII_ANLPAR             0x05
#define BMSR_LSTATUS           (1u << 2)
#define BMSR_ANEGCOMPLETE      (1u << 5)

static void puts_hex32(unsigned int v)
{
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        unsigned int nyb = (v >> ((7 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10));
    }
    buf[10] = 0;
    uart_puts(buf);
}

unsigned int genet_sys_rev(void)         { return GENET_REG(SYS_REV_CTRL); }
unsigned int genet_sys_port_ctrl(void)   { return GENET_REG(SYS_PORT_CTRL); }

void genet_get_mac(unsigned char mac[6])
{
    /* UMAC stores the MAC in two 32-bit slots:
     *   UMAC_MAC0 = (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5]
     *   UMAC_MAC1 = (mac[0] <<  8) |  mac[1]
     * — i.e. big-endian byte order with bytes 4..5 in mac0 LSBs.
     * (Linux: bcmgenet_umac_set_hw_addr / get_hw_addr.) */
    unsigned int m0 = GENET_REG(UMAC_MAC0);
    unsigned int m1 = GENET_REG(UMAC_MAC1);
    mac[0] = (unsigned char)((m1 >> 8)  & 0xFF);
    mac[1] = (unsigned char)( m1        & 0xFF);
    mac[2] = (unsigned char)((m0 >> 24) & 0xFF);
    mac[3] = (unsigned char)((m0 >> 16) & 0xFF);
    mac[4] = (unsigned char)((m0 >> 8)  & 0xFF);
    mac[5] = (unsigned char)( m0        & 0xFF);
}

static void genet_set_mac(const unsigned char mac[6])
{
    unsigned int m0 = ((unsigned int)mac[2] << 24)
                    | ((unsigned int)mac[3] << 16)
                    | ((unsigned int)mac[4] <<  8)
                    |  (unsigned int)mac[5];
    unsigned int m1 = ((unsigned int)mac[0] <<  8) | (unsigned int)mac[1];
    GENET_REG(UMAC_MAC0) = m0;
    GENET_REG(UMAC_MAC1) = m1;
}

/* VC mailbox tag 0x00010003 (get-board-mac-address) — firmware
 * reads the OTP and returns the canonical 6-byte MAC.  Reliable
 * even if UMAC_MAC0/1 were cleared by a SW reset. */
static int genet_get_mac_via_mailbox(unsigned char mac[6])
{
    static volatile unsigned int __attribute__((aligned(16))) buf[8];
    buf[0] = 32;
    buf[1] = 0;
    buf[2] = 0x00010003u;
    buf[3] = 6;              /* response value buffer size in bytes */
    buf[4] = 0;
    buf[5] = 0;
    buf[6] = 0;
    buf[7] = 0;
    if (mbox_call(buf) != 0) return -1;
    /* Response layout: buf[5] = mac[0..3] (little-endian, byte 0 at LSB),
     *                  buf[6] = mac[4..5]. */
    mac[0] = (unsigned char)( buf[5]        & 0xFF);
    mac[1] = (unsigned char)((buf[5] >>  8) & 0xFF);
    mac[2] = (unsigned char)((buf[5] >> 16) & 0xFF);
    mac[3] = (unsigned char)((buf[5] >> 24) & 0xFF);
    mac[4] = (unsigned char)( buf[6]        & 0xFF);
    mac[5] = (unsigned char)((buf[6] >>  8) & 0xFF);
    return 0;
}

static void puts_mac(const unsigned char mac[6])
{
    for (int i = 0; i < 6; i++) {
        unsigned char b = mac[i];
        unsigned char hi = (unsigned char)((b >> 4) & 0xF);
        unsigned char lo = (unsigned char)(b & 0xF);
        uart_putc((char)(hi < 10 ? '0' + hi : 'a' + hi - 10));
        uart_putc((char)(lo < 10 ? '0' + lo : 'a' + lo - 10));
        if (i < 5) uart_putc(':');
    }
}

static inline void udelay_busy(unsigned long us)
{
    unsigned long freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long target = (freq / 1000000UL) * us;
    unsigned long start, now;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));
    do {
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
    } while (now - start < target);
}

static int mdio_read(unsigned phy_id, unsigned reg)
{
    /* Issue: PHY_ID + REG, then assert START_BUSY.  HW clears it
     * when the access completes (~70 µs at the MDIO clock).  Bound
     * the wait via CNTPCT_EL0 — never spin on bare MMIO polls,
     * because if MDIO clocks aren't running the bit never clears
     * and the kernel hangs. */
    unsigned int cmd = ((phy_id & 0x1F) << MDIO_PHY_ID_SHIFT)
                     | ((reg    & 0x1F) << MDIO_REG_SHIFT)
                     |  MDIO_RD;
    GENET_REG(UMAC_MDIO_CMD) = cmd;
    unsigned int reg2 = GENET_REG(UMAC_MDIO_CMD);
    reg2 |= MDIO_START_BUSY;
    GENET_REG(UMAC_MDIO_CMD) = reg2;

    /* Timeout: 50 ms in CNTPCT ticks. */
    unsigned long freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long target = (freq / 1000UL) * 50UL;
    unsigned long start, now;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(start));

    while (1) {
        unsigned int v = GENET_REG(UMAC_MDIO_CMD);
        if (!(v & MDIO_START_BUSY)) {
            if (v & MDIO_READ_FAIL) return -1;
            return (int)(v & MDIO_DATA_MASK);
        }
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(now));
        if (now - start >= target) return -2;   /* timeout */
    }
}

/* forward decls */
static void genet_send_one_arp(const unsigned char src_mac[6]);
static void genet_init_rx(void);
void genet_rx_recover(void);

static void umac_soft_reset(void)
{
    /* Mirrors Linux bcmgenet_umac_reset() but skips the RBUF_CTRL
     * preamble (it was hanging us last build).  The LCL_LOOP_EN bit
     * keeps the rxclk stable during the SW reset window — without
     * it the UMAC can leave reset in a state where subsequent
     * register writes (e.g. UMAC_MAC0) stall the bus. */
    GENET_REG(UMAC_CMD) = 0;
    GENET_REG(UMAC_CMD) = CMD_SW_RESET | CMD_LCL_LOOP_EN;
    for (volatile int i = 0; i < 1000; i++) { }
    GENET_REG(UMAC_CMD) = 0;
}

void genet_init(void)
{
    uart_puts("genet: BCM2711 GENET base = ");
    puts_hex32((unsigned int)GENET_BASE);
    uart_puts("\n");

    unsigned int rev   = GENET_REG(SYS_REV_CTRL);
    unsigned int pctl  = GENET_REG(SYS_PORT_CTRL);
    unsigned int rflsh = GENET_REG(SYS_RBUF_FLUSH_CTRL);
    unsigned int tflsh = GENET_REG(SYS_TBUF_FLUSH_CTRL);

    uart_puts("  SYS_REV_CTRL        = "); puts_hex32(rev);   uart_puts("\n");
    uart_puts("  SYS_PORT_CTRL       = "); puts_hex32(pctl);  uart_puts("\n");
    uart_puts("  SYS_RBUF_FLUSH_CTRL = "); puts_hex32(rflsh); uart_puts("\n");
    uart_puts("  SYS_TBUF_FLUSH_CTRL = "); puts_hex32(tflsh); uart_puts("\n");

    if (rev == 0 || rev == 0xFFFFFFFFu) {
        uart_puts("genet: controller MMIO not responding\n");
        return;
    }
    /* GENET v5 on BCM2711: top byte of SYS_REV_CTRL is the GENET major. */
    unsigned int major = (rev >> 24) & 0xFFu;
    uart_puts("genet: GENET major rev = ");
    puts_hex32(major);
    uart_puts("  (expected 0x06 on Pi 4)\n");

    /* NET-B — read MAC *before* the SW reset (firmware may have
     * preloaded UMAC_MAC0/1), then reset UMAC, then write the MAC
     * back so RX filtering and DA insertion work. */
    unsigned char mac_pre[6];
    genet_get_mac(mac_pre);
    uart_puts("genet: MAC pre-reset = ");
    puts_mac(mac_pre);
    uart_puts("\n");

    /* MAC fallback chain:
     *   1. firmware-loaded UMAC_MAC0/1 (when firmware filled them)
     *   2. otherwise local-admin synthetic 02:00:00:00:00:01
     *
     * Note: VC mailbox 0x00010003 (get-board-mac-address) used to
     * be tried here but calling it then immediately writing UMAC
     * registers triggered a bus hang on Pi 4 — possibly some bus
     * fence state the mailbox leaves behind.  Skipping it lets the
     * UMAC reset proceed cleanly. */
    int is_zero = 1;
    for (int i = 0; i < 6; i++) if (mac_pre[i]) { is_zero = 0; break; }
    unsigned char mac[6];
    if (!is_zero) {
        for (int i = 0; i < 6; i++) mac[i] = mac_pre[i];
    } else {
        uart_puts("genet: pre-reset MAC is all-zero; using local-admin 02:00:00:00:00:01\n");
        mac[0] = 0x02; mac[1] = 0x00; mac[2] = 0x00;
        mac[3] = 0x00; mac[4] = 0x00; mac[5] = 0x01;
    }

    /* SKIP umac_soft_reset and genet_set_mac on Pi 4.
     *
     * Empirically: with Pi 4 firmware's GENET state, writing
     * UMAC_MAC0 stalls the AXI bus indefinitely.  Firmware has
     * already configured UMAC to a usable state, so we don't
     * actually need to reset it here.  When we eventually need
     * the source MAC we'll put it in the Ethernet header
     * directly rather than relying on UMAC's DA insertion. */
    uart_puts("genet: skipping UMAC reset + MAC write (firmware-initialised)\n");
    (void)mac;

    /* NET-C1 — read BCM54213PE PHY via MDIO so we know the link
     * is up before we bother with TDMA ring init.  PHY ID is 1
     * on Pi 4 (only one PHY on the bus).
     *
     * Note: any single mdio_read() that times out returns -2; we
     * print that as -0x2 so the failure mode is visible rather
     * than freezing the kernel. */
    uart_puts("genet: probing PHY 1 ...\n");
    int phyid1 = mdio_read(PHY_ID_BCM54213PE, MII_PHYSID1);
    uart_puts("genet/phy: PHYSID1 = "); puts_hex32((unsigned)phyid1); uart_puts("\n");
    int phyid2 = mdio_read(PHY_ID_BCM54213PE, MII_PHYSID2);
    uart_puts("genet/phy: PHYSID2 = "); puts_hex32((unsigned)phyid2); uart_puts("\n");
    int bmcr   = mdio_read(PHY_ID_BCM54213PE, MII_BMCR);
    uart_puts("genet/phy: BMCR    = "); puts_hex32((unsigned)bmcr);   uart_puts("\n");
    int bmsr   = mdio_read(PHY_ID_BCM54213PE, MII_BMSR);
    uart_puts("genet/phy: BMSR    = "); puts_hex32((unsigned)bmsr);   uart_puts("\n");
    /* BCM54213PE OUI 0x001818 should appear in phyid1/phyid2:
     * PHYSID1 = 0x600D, PHYSID2 = 0x84A2 (BCM54213PE). */
    if (bmsr >= 0) {
        uart_puts("genet/phy: link  = ");
        uart_puts((bmsr & BMSR_LSTATUS) ? "UP\n" : "DOWN\n");
        uart_puts("genet/phy: aneg  = ");
        uart_puts((bmsr & BMSR_ANEGCOMPLETE) ? "complete\n" : "in progress\n");
    }

    /* BMSR.Link Status is *latched low* — the first read clears the
     * latched bit and the second shows the actual current state.
     * Poll a couple of times so we don't miss a link that comes
     * up a fraction of a second after the kernel banner. */
    int link_up = 0;
    for (int t = 0; t < 3; t++) {
        udelay_busy(200000UL);   /* 200 ms */
        int bmsr2 = mdio_read(PHY_ID_BCM54213PE, MII_BMSR);
        uart_puts("genet/phy: BMSR (poll ");
        uart_putc((char)('1' + t));
        uart_puts(") = ");
        puts_hex32((unsigned)bmsr2);
        if (bmsr2 >= 0 && (bmsr2 & BMSR_LSTATUS)) {
            uart_puts("  link=UP\n");
            link_up = 1;
            break;
        }
        uart_puts("  link=DOWN\n");
    }
    (void)link_up;

    if (!link_up) {
        uart_puts("genet: link still DOWN — skipping TX setup\n");
        return;
    }

    /* Read BCM54213PE Auxiliary Status Summary (MII reg 0x19) to
     * find out what speed/duplex the PHY actually negotiated with
     * the link partner.  Bits 10:8 (HCD = Highest Common Denominator):
     *   001 = 10BASE-T HD     010 = 10BASE-T FD
     *   011 = 100BASE-TX HD   100 = 100BASE-TX FD
     *   110 = 1000BASE-T HD   111 = 1000BASE-T FD                     */
    int aux = mdio_read(PHY_ID_BCM54213PE, 0x19);
    g_diag_aux_stat = (unsigned)aux;
    uart_puts("genet/phy: AUX_STAT(0x19) = ");
    puts_hex32((unsigned)aux);
    uart_puts("\n");
    unsigned int umac_speed = CMD_SPEED_1000;
    if (aux >= 0) {
        unsigned hcd = ((unsigned)aux >> 8) & 0x7;
        uart_puts("genet/phy: HCD = ");
        uart_putc((char)('0' + hcd));
        uart_puts(" (");
        switch (hcd) {
        case 1: uart_puts("10BT-HD");      umac_speed = CMD_SPEED_10; break;
        case 2: uart_puts("10BT-FD");      umac_speed = CMD_SPEED_10; break;
        case 3: uart_puts("100BTX-HD");    umac_speed = CMD_SPEED_100; break;
        case 4: uart_puts("100BTX-FD");    umac_speed = CMD_SPEED_100; break;
        case 6: uart_puts("1000BT-HD");    umac_speed = CMD_SPEED_1000; break;
        case 7: uart_puts("1000BT-FD");    umac_speed = CMD_SPEED_1000; break;
        default: uart_puts("unknown");     break;
        }
        uart_puts(")\n");
    }
    g_umac_speed_bits = umac_speed;

    /* ====================================================== *
     * NET-C3 — send one broadcast ARP frame via TDMA ring 16 *
     * ====================================================== */
    genet_send_one_arp(mac);

    /* NET-D — initialise RX path so future broadcasts / ARP
     * replies start landing in rx_bufs[]. */
    genet_init_rx();
}

/* ---- RX path (NET-D / NET-G hardening) ----
 *
 * The RX ring is now the full hardware descriptor count (256).  The
 * earlier 16-deep ring (32 KB) could not survive a TCP burst: the
 * dispatcher only drains once per window-manager frame (~50 ms at
 * 20 fps), so when `nc` opened a connection the ring backed up, the
 * on-chip RBUF FIFO overflowed, and the RX path wedged — which looked
 * like "the ICMP responder stopped" because we could no longer even
 * receive the echo requests.  256 × 2048 B = 512 KB in BSS (NOLOAD,
 * so it costs nothing in the image, only a one-time boot zero).
 *
 * The HW puts incoming Ethernet frames into rx_bufs[i] and updates
 * the descriptor's length_status; we poll PROD_INDEX to detect new
 * arrivals.  After consumption we bump CONS_INDEX and re-arm the
 * descriptor with DMA_OWN.  genet_rx_recover() resynchronises the
 * ring if HW ever outruns the software consumer. */

#define RX_RING_DESCS               256

static unsigned char __attribute__((aligned(64)))
    rx_bufs[RX_RING_DESCS][RX_BUF_LENGTH];

static unsigned long g_rx_packets;
static unsigned long g_rx_bytes;
static unsigned int  g_rx_cons;       /* SW-side consumer index 0..RING-1 */
static int           g_rx_inited;
static unsigned long g_rx_overruns;   /* times HW outran us (ring resync) */
static unsigned long g_rx_recoveries; /* RBUF flush + ring re-arm count */
static unsigned long g_tx_timeouts;   /* TX completions that never landed */

/* TX buffer in DRAM — descriptor itself lives in GENET MMIO,
 * but the packet payload is in main memory and its address is
 * stored in the descriptor's ADDRESS_LO/HI words. */
static unsigned char __attribute__((aligned(64))) tx_buf[64] = {
    /* Ethernet header */
    0xff,0xff,0xff,0xff,0xff,0xff,                  /* dst broadcast */
    0x00,0x00,0x00,0x00,0x00,0x00,                  /* src (patched) */
    0x08,0x06,                                       /* EtherType ARP */
    /* ARP payload */
    0x00,0x01,                                       /* HTYPE Ethernet */
    0x08,0x00,                                       /* PTYPE IPv4 */
    0x06,                                            /* HLEN */
    0x04,                                            /* PLEN */
    0x00,0x01,                                       /* OPER request */
    0x00,0x00,0x00,0x00,0x00,0x00,                  /* SHA (patched) */
    0x00,0x00,0x00,0x00,                             /* SPA 0.0.0.0 */
    0x00,0x00,0x00,0x00,0x00,0x00,                  /* THA */
    192,168,1,1,                                     /* TPA */
    /* zero pad to 60 bytes */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static void genet_send_one_arp(const unsigned char src_mac[6])
{
    uart_puts("genet/tx: NET-C3 sending one broadcast ARP (U-Boot path)\n");
    unsigned int len = 60;

    /* Stage 1 — common UMAC / RBUF / RGMII bring-up.
     * Skipped UMAC reset because UMAC_MAC0 writes still stall.
     * Pi 4 firmware left UMAC working enough to accept CMD writes. */
    GENET_REG(SYS_PORT_CTRL)       = SYS_PORT_MODE_EXT_GPHY;
    GENET_REG(SYS_RBUF_FLUSH_CTRL) = 0;
    GENET_REG(SYS_TBUF_FLUSH_CTRL) = 0;
    unsigned int ext = GENET_REG(EXT_RGMII_OOB_CTRL);
    g_diag_rgmii_before = ext;
    uart_puts("genet/ext: RGMII_OOB before = "); puts_hex32(ext); uart_puts("\n");
    ext |= EXT_RGMII_MODE_EN | EXT_ID_MODE_DIS | EXT_RGMII_LINK;
    ext &= ~EXT_OOB_DISABLE;
    GENET_REG(EXT_RGMII_OOB_CTRL)  = ext;
    __asm__ volatile ("dsb sy" ::: "memory");
    g_diag_rgmii_after = GENET_REG(EXT_RGMII_OOB_CTRL);
    uart_puts("genet/ext: RGMII_OOB after  = "); puts_hex32(g_diag_rgmii_after); uart_puts("\n");
    /* UMAC speed must match the negotiated PHY link speed.  Pi 4
     * with the integrated BCM54213PE PHY auto-negotiates to 1000
     * Mbps full-duplex when connected to a gigabit switch.  If the
     * speed bits are 0 (10 Mbps) but the line is 1000 Mbps, every
     * received frame is reported with CRC/LG/RXER errors. */
    /* Add PROMISC so we receive every frame on the wire (helps
     * diagnose whether HW filter is rejecting our intended traffic)
     * and PAD_EN+CRC_FWD so short and CRC-carrying frames don't get
     * dropped silently. */
    unsigned int cmd_val = CMD_TX_EN | CMD_RX_EN | g_umac_speed_bits
                         | CMD_PROMISC | CMD_PAD_EN | CMD_CRC_FWD;
    GENET_REG(UMAC_CMD)            = cmd_val;
    g_diag_umac_cmd = cmd_val;
    uart_puts("genet/umac: UMAC_CMD = ");
    puts_hex32(cmd_val);
    uart_puts(" (PROMISC|PAD|CRC_FWD|speed_bits)\n");
    __asm__ volatile ("dsb sy" ::: "memory");
    uart_puts("genet/tx: UMAC_CMD = "); puts_hex32(GENET_REG(UMAC_CMD)); uart_puts("\n");

    /* Stage 2 — TX ring init, U-Boot style.
     * - START_ADDR/READ_PTR/WRITE_PTR all 0 (descriptors live at
     *   the *start* of the TX block at GENET_TX_OFF, the ring
     *   register START_ADDR is an offset from there).
     * - END_ADDR = TX_DESCS * DMA_DESC_SIZE / 4 - 1 in word units.
     * - PROD_INDEX is initialised to the CURRENT CONS_INDEX
     *   (which firmware may have set non-zero) so the ring starts
     *   in "all sent" state.  Subsequent TX increments PROD only. */
    GENET_REG(TX_DMA_TOP + 0x0C /*SCB_BURST_SIZE*/) = 8;
    GENET_REG(TX_RING_BASE + TDMA_START_ADDR_LO) = 0;
    GENET_REG(TX_RING_BASE + TDMA_READ_PTR_LO)   = 0;
    GENET_REG(TX_RING_BASE + TDMA_WRITE_PTR_LO)  = 0;
    GENET_REG(TX_RING_BASE + TDMA_END_ADDR_LO)   = TX_DESCS * DMA_DESC_SIZE / 4 - 1;
    unsigned int cur_cons = GENET_REG(TX_RING_BASE + TDMA_CONS_INDEX);
    GENET_REG(TX_RING_BASE + TDMA_PROD_INDEX)        = cur_cons;
    GENET_REG(TX_RING_BASE + TDMA_MBUF_DONE_THRESH)  = 1;
    GENET_REG(TX_RING_BASE + TDMA_FLOW_PERIOD)       = 0;
    GENET_REG(TX_RING_BASE + TDMA_RING_BUF_SIZE)     = (TX_DESCS << 16) | 2048;
    GENET_REG(TX_DMA_TOP + 0x00 /*DMA_RING_CFG*/)    = 1u << TX_DEFAULT_RING;
    __asm__ volatile ("dsb sy" ::: "memory");

    /* Stage 3 — write TX descriptor #0 *into MMIO* (NOT DRAM).
     * Descriptor area lives at GENET_TX_OFF .. + 256*12 bytes,
     * each descriptor 12 bytes (3 words). */
    unsigned int  tx_index = cur_cons & 0xFF;
    unsigned int  desc_off = GENET_TX_OFF + tx_index * DMA_DESC_SIZE;
    unsigned long buf_pa   = (unsigned long)tx_buf;
    unsigned int  len_stat = ((unsigned int)len << DMA_BUFLENGTH_SHIFT)
                           | (DMA_TX_DEFAULT_QTAG << DMA_TX_QTAG_SHIFT)
                           | DMA_TX_APPEND_CRC | DMA_SOP | DMA_EOP;
    GENET_REG(desc_off + DMA_DESC_ADDRESS_LO)     = (unsigned int)(buf_pa & 0xFFFFFFFFu);
    GENET_REG(desc_off + DMA_DESC_ADDRESS_HI)     = (unsigned int)((buf_pa >> 32) & 0xFFFFFFFFu);
    GENET_REG(desc_off + DMA_DESC_LENGTH_STATUS)  = len_stat;
    __asm__ volatile ("dsb sy" ::: "memory");
    uart_puts("genet/tx: desc[0]@MMIO len_stat=");
    puts_hex32(len_stat);
    uart_puts(" buf_lo=");
    puts_hex32((unsigned int)(buf_pa & 0xFFFFFFFFu));
    uart_puts("\n");

    /* Stage 4 — enable DMA: bit (DEFAULT_Q + 1) per-ring + DMA_EN bit 0 */
    GENET_REG(TX_DMA_TOP + 0x04 /*DMA_CTRL*/) = (1u << (TX_DEFAULT_RING + 1)) | 1u;
    __asm__ volatile ("dsb sy" ::: "memory");
    uart_puts("genet/tx: TDMA_CTRL = ");
    puts_hex32(GENET_REG(TX_DMA_TOP + 0x04));
    uart_puts("\n");

    /* Stage 5 — bump PROD_INDEX to hand descriptor to DMA */
    unsigned int new_prod = cur_cons + 1;
    GENET_REG(TX_RING_BASE + TDMA_PROD_INDEX) = new_prod;
    __asm__ volatile ("dsb sy" ::: "memory");
    uart_puts("genet/tx: PROD now ");
    puts_hex32(new_prod);
    uart_puts(", waiting...\n");

    /* Stage 6 — poll CONS_INDEX with 200 ms timeout */
    unsigned long freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long target = (freq / 1000UL) * 200UL;
    unsigned long t_start, t_now;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t_start));
    unsigned int cons = cur_cons;
    while (1) {
        cons = GENET_REG(TX_RING_BASE + TDMA_CONS_INDEX) & 0xFFFFu;
        if (cons == (new_prod & 0xFFFFu)) break;
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t_now));
        if (t_now - t_start >= target) break;
    }
    uart_puts("genet/tx: CONS = ");
    puts_hex32(cons);
    uart_puts((cons == (new_prod & 0xFFFFu)) ? "  (sent OK)\n" : "  (TX timeout)\n");
    if (cons != (new_prod & 0xFFFFu)) {
        uart_puts("genet/tx: CTRL/RING_CFG = ");
        puts_hex32(GENET_REG(TX_DMA_TOP + 0x04));
        uart_puts(" / ");
        puts_hex32(GENET_REG(TX_DMA_TOP + 0x00));
        uart_puts("\n");
    }
}

static void genet_init_rx(void)
{
    uart_puts("genet/rx: trace A (entering init_rx)\n");

    /* Fill rx_bufs with 0xAA so we can tell whether HW actually
     * writes received frames here.  If the dump shows 0xAA bytes
     * later, HW didn't DMA to our buffer. */
    for (int i = 0; i < RX_RING_DESCS; i++) {
        for (int j = 0; j < 32; j++) rx_bufs[i][j] = 0xAA;
    }
    __asm__ volatile ("dsb sy" ::: "memory");
    uart_puts("genet/rx: rx_bufs[0]@");
    {
        unsigned long a = (unsigned long)&rx_bufs[0][0];
        for (int i = 7; i >= 0; i--) {
            unsigned int n = (a >> (i * 4)) & 0xF;
            uart_putc((char)(n < 10 ? '0' + n : 'a' + n - 10));
        }
    }
    uart_puts(" first=");
    for (int j = 0; j < 4; j++) {
        unsigned char b = rx_bufs[0][j];
        uart_putc("0123456789abcdef"[(b >> 4) & 0xF]);
        uart_putc("0123456789abcdef"[ b       & 0xF]);
    }
    uart_puts(" (expect aaaaaaaa)\n");

    /* Make sure RBUF doesn't prepend extra bytes (RBUF_ALIGN_2B
     * or BRCM tag) and that UMAC RX path isn't stuck flushed. */
    uart_puts("genet/rx: RBUF_CTRL before = ");
    puts_hex32(GENET_REG(RBUF_CTRL));
    uart_puts("\n");
    GENET_REG(RBUF_CTRL) = 0;
    __asm__ volatile ("dsb sy" ::: "memory");

    /* 1. Populate RX descriptors.  Each descriptor points to its
     * own buffer in rx_bufs[].  RX descriptors need DMA_OWN set
     * so HW knows they're available to receive into. */
    for (int i = 0; i < RX_RING_DESCS; i++) {
        unsigned int desc_off = GENET_RX_OFF + i * DMA_DESC_SIZE;
        unsigned long buf_pa  = (unsigned long)rx_bufs[i];
        GENET_REG(desc_off + DMA_DESC_ADDRESS_LO)
            = (unsigned int)(buf_pa & 0xFFFFFFFFu);
        GENET_REG(desc_off + DMA_DESC_ADDRESS_HI)
            = (unsigned int)((buf_pa >> 32) & 0xFFFFFFFFu);
        GENET_REG(desc_off + DMA_DESC_LENGTH_STATUS)
            = (RX_BUF_LENGTH << DMA_BUFLENGTH_SHIFT) | DMA_OWN;
    }

    uart_puts("genet/rx: trace B (descriptors written)\n");

    /* 2. SCB burst size */
    GENET_REG(RX_DMA_TOP + 0x0C /*DMA_SCB_BURST_SIZE*/) = 8;

    /* 3. Per-ring RX setup */
    GENET_REG(RX_RING_BASE + TDMA_START_ADDR_LO) = 0;
    GENET_REG(RX_RING_BASE + RDMA_WRITE_PTR)     = 0;
    GENET_REG(RX_RING_BASE + RDMA_READ_PTR)      = 0;
    GENET_REG(RX_RING_BASE + TDMA_END_ADDR_LO)
        = RX_RING_DESCS * DMA_DESC_SIZE / 4 - 1;
    unsigned int cur_prod = GENET_REG(RX_RING_BASE + RDMA_PROD_INDEX);
    GENET_REG(RX_RING_BASE + RDMA_CONS_INDEX) = cur_prod;
    g_rx_cons = cur_prod & 0xFF;
    if (g_rx_cons >= RX_RING_DESCS) g_rx_cons %= RX_RING_DESCS;
    GENET_REG(RX_RING_BASE + TDMA_MBUF_DONE_THRESH) = 1;
    GENET_REG(RX_RING_BASE + TDMA_RING_BUF_SIZE)
        = (RX_RING_DESCS << 16) | RX_BUF_LENGTH;
    /* Flow-control threshold (XOFF<<16 | XON) — Linux default. */
    GENET_REG(RX_RING_BASE + RDMA_XON_XOFF_THRESH) = (5u << 16) | (RX_RING_DESCS >> 4);

    uart_puts("genet/rx: trace C (ring regs written)\n");

    /* 4. Enable ring 16 + global DMA in RX top-level */
    GENET_REG(RX_DMA_TOP + 0x00 /*DMA_RING_CFG*/) = 1u << TX_DEFAULT_RING;
    GENET_REG(RX_DMA_TOP + 0x04 /*DMA_CTRL*/)
        = (1u << (TX_DEFAULT_RING + 1)) | 1u;

    __asm__ volatile ("dsb sy" ::: "memory");

    g_rx_inited = 1;
    uart_puts("genet/rx: NET-D RX ring armed (16 descs, 32 KB buffer pool)\n");

    /* After ring enabled, dump descriptor 0's state to verify we
     * wrote the right address.  If desc0.ADDRESS_LO != rx_bufs[0]
     * address, our writes are landing somewhere else. */
    {
        unsigned int ls  = GENET_REG(GENET_RX_OFF + 0 * DMA_DESC_SIZE + DMA_DESC_LENGTH_STATUS);
        unsigned int alo = GENET_REG(GENET_RX_OFF + 0 * DMA_DESC_SIZE + DMA_DESC_ADDRESS_LO);
        unsigned int ahi = GENET_REG(GENET_RX_OFF + 0 * DMA_DESC_SIZE + DMA_DESC_ADDRESS_HI);
        uart_puts("genet/rx: desc0 len_stat=");
        puts_hex32(ls);
        uart_puts(" addr=");
        puts_hex32(ahi); puts_hex32(alo);
        uart_puts("\n");
    }
}

/* Recover a wedged / overrun RX path: flush the on-chip RBUF FIFO,
 * re-arm every descriptor with DMA_OWN, and snap the software consumer
 * index back to the hardware producer index.  Invoked when an overrun
 * is detected (see genet_rx_poll) so a transient burst — e.g. the SYN
 * storm when a TCP client connects — can never permanently stall RX.
 * This is the RX-side analogue of the TX MMU reset used on smc91c111. */
void genet_rx_recover(void)
{
    if (!g_rx_inited) return;

    /* Pulse the RX SRAM flush so a backed-up FIFO is cleared. */
    GENET_REG(SYS_RBUF_FLUSH_CTRL) = 1;
    __asm__ volatile ("dsb sy" ::: "memory");
    GENET_REG(SYS_RBUF_FLUSH_CTRL) = 0;
    __asm__ volatile ("dsb sy" ::: "memory");

    /* Re-arm all descriptors so HW owns the whole ring again. */
    for (int i = 0; i < RX_RING_DESCS; i++) {
        unsigned int desc_off = GENET_RX_OFF + i * DMA_DESC_SIZE;
        GENET_REG(desc_off + DMA_DESC_LENGTH_STATUS)
            = (RX_BUF_LENGTH << DMA_BUFLENGTH_SHIFT) | DMA_OWN;
    }

    /* Snap CONS up to PROD (ring now empty) and re-derive the SW index
     * so rx_bufs[g_rx_cons] tracks the next descriptor HW will fill. */
    unsigned int prod = GENET_REG(RX_RING_BASE + RDMA_PROD_INDEX);
    GENET_REG(RX_RING_BASE + RDMA_CONS_INDEX) = prod;
    g_rx_cons = prod % RX_RING_DESCS;
    __asm__ volatile ("dsb sy" ::: "memory");

    g_rx_recoveries++;
}

int genet_rx_poll(unsigned char **out_pkt)
{
    if (!g_rx_inited) return 0;
    unsigned int prod = GENET_REG(RX_RING_BASE + RDMA_PROD_INDEX) & 0xFFFFu;
    unsigned int cons = GENET_REG(RX_RING_BASE + RDMA_CONS_INDEX) & 0xFFFFu;
    if (prod == cons) return 0;

    /* Overrun guard.  If HW has advanced PROD more than a full ring
     * past our CONS, the descriptor we are about to read was already
     * recycled by HW and rx_bufs[idx] no longer matches the frame the
     * hardware thinks it delivered.  Resynchronise instead of handing
     * back a stale/garbled buffer — this silent desync was exactly
     * what wedged the responder after a TCP burst. */
    unsigned int avail = (prod - cons) & 0xFFFFu;
    if (avail > RX_RING_DESCS) {
        g_rx_overruns++;
        genet_rx_recover();
        return 0;
    }

    /* Descriptor that HW just filled. */
    unsigned int idx       = g_rx_cons;
    unsigned int desc_off  = GENET_RX_OFF + idx * DMA_DESC_SIZE;
    unsigned int len_stat  = GENET_REG(desc_off + DMA_DESC_LENGTH_STATUS);
    unsigned int length    = (len_stat >> DMA_BUFLENGTH_SHIFT) & 0x0FFF;

    /* Deliberately NO logging in this hot loop: uart_puts() drives the
     * slow framebuffer console, and any printing here lengthens the
     * drain so much that the ring overflows under load.  Use the
     * counter accessors (genet_rx_overrun_count, …) for diagnosis. */

    if (out_pkt) *out_pkt = rx_bufs[idx];
    g_rx_packets++;
    g_rx_bytes += length;
    return (int)length;
}

void genet_rx_release(void)
{
    if (!g_rx_inited) return;
    /* Re-arm descriptor with DMA_OWN so HW can refill it. */
    unsigned int idx      = g_rx_cons;
    unsigned int desc_off = GENET_RX_OFF + idx * DMA_DESC_SIZE;
    GENET_REG(desc_off + DMA_DESC_LENGTH_STATUS)
        = (RX_BUF_LENGTH << DMA_BUFLENGTH_SHIFT) | DMA_OWN;

    /* Advance CONS_INDEX (HW-visible) and software index. */
    unsigned int cons = GENET_REG(RX_RING_BASE + RDMA_CONS_INDEX);
    GENET_REG(RX_RING_BASE + RDMA_CONS_INDEX) = cons + 1;
    g_rx_cons = (g_rx_cons + 1) % RX_RING_DESCS;
    __asm__ volatile ("dsb sy" ::: "memory");
}

unsigned long genet_rx_packet_count(void)  { return g_rx_packets;    }
unsigned long genet_rx_byte_count(void)    { return g_rx_bytes;      }
unsigned long genet_rx_overrun_count(void) { return g_rx_overruns;   }
unsigned long genet_rx_recover_count(void) { return g_rx_recoveries; }
unsigned long genet_tx_timeout_count(void) { return g_tx_timeouts;   }

unsigned int genet_phy_bmsr(void)
{
    int v = mdio_read(PHY_ID_BCM54213PE, MII_BMSR);
    return (v < 0) ? 0xFFFFFFFFu : (unsigned int)v;
}

int genet_link_up(void)
{
    unsigned int b = genet_phy_bmsr();
    if (b == 0xFFFFFFFFu) return 0;
    return (b & BMSR_LSTATUS) ? 1 : 0;
}

/* Track TX descriptor index across calls so subsequent TX frames
 * use a fresh descriptor slot. */
static unsigned int g_tx_index_sw;
static int          g_tx_inited;
static unsigned char __attribute__((aligned(64))) tx_buf_pool[1518];

int genet_tx_frame(const unsigned char *frame, int length)
{
    if (length <= 0 || length > 1518) return -1;

    /* Copy caller's frame into the DMA-aligned buffer. */
    for (int i = 0; i < length; i++) tx_buf_pool[i] = frame[i];

    if (!g_tx_inited) {
        g_tx_inited = 1;
        unsigned int c = GENET_REG(TX_RING_BASE + TDMA_CONS_INDEX);
        g_tx_index_sw = c & 0xFF;
    }

    unsigned int idx       = g_tx_index_sw % TX_DESCS;
    unsigned int desc_off  = GENET_TX_OFF + idx * DMA_DESC_SIZE;
    unsigned long buf_pa   = (unsigned long)tx_buf_pool;
    unsigned int len_stat  = ((unsigned int)length << DMA_BUFLENGTH_SHIFT)
                           | (DMA_TX_DEFAULT_QTAG << DMA_TX_QTAG_SHIFT)
                           | DMA_TX_APPEND_CRC | DMA_SOP | DMA_EOP;
    GENET_REG(desc_off + DMA_DESC_ADDRESS_LO)    = (unsigned int)(buf_pa & 0xFFFFFFFFu);
    GENET_REG(desc_off + DMA_DESC_ADDRESS_HI)    = (unsigned int)((buf_pa >> 32) & 0xFFFFFFFFu);
    GENET_REG(desc_off + DMA_DESC_LENGTH_STATUS) = len_stat;
    __asm__ volatile ("dsb sy" ::: "memory");
    unsigned int prod = GENET_REG(TX_RING_BASE + TDMA_PROD_INDEX) + 1;
    GENET_REG(TX_RING_BASE + TDMA_PROD_INDEX) = prod;
    g_tx_index_sw = (g_tx_index_sw + 1) & 0xFFFF;
    __asm__ volatile ("dsb sy" ::: "memory");

    unsigned long freq;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    unsigned long target = (freq / 1000UL) * 50UL;
    unsigned long t0, tn;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t0));
    while (1) {
        unsigned int cons = GENET_REG(TX_RING_BASE + TDMA_CONS_INDEX) & 0xFFFF;
        if (cons == (prod & 0xFFFF)) return 0;
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(tn));
        if (tn - t0 >= target) {
            /* TX did not complete in time.  Just report failure — do
             * NOT poke the GENET DMA control/PROD registers to "recover"
             * here: writing those while the controller is in a bad state
             * can stall the AXI bus indefinitely on this Pi 4 (the same
             * hazard as the UMAC_MAC0 write), which hard-hangs the whole
             * box.  Earlier ICMP-only traffic never timed out, so the
             * hang only appeared once a TCP SYN+ACK drove this path. */
            g_tx_timeouts++;
            return -1;
        }
    }
}

#endif /* GENET_BASE */
