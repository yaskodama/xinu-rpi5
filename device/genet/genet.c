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
#define CMD_LCL_LOOP_EN        (1u << 15)
#define CMD_SW_RESET           (1u << 13)
#define RBUF_CTRL              0x300

/* MDIO command register inside UMAC: UMAC_BASE + 0x614 = 0xE14 */
#define UMAC_MDIO_CMD          (UMAC_BASE + 0x614)
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

    /* NET-C2 — probe the TDMA register area at 0x4000..0x4FFF.
     * Dump every 64-byte-aligned word whose value isn't 0 or all-
     * ones so we can identify where the ring registers live on
     * BCM2711 GENET v5 (Linux bcmgenet.h is our reference, but
     * exact offsets are easier to verify against real silicon). */
    uart_puts("genet/dma: probing TDMA 0x4000-0x4FFF (non-trivial only)\n");
    for (unsigned int off = 0x4000; off < 0x5000; off += 4) {
        unsigned int v = GENET_REG(off);
        if (v != 0 && v != 0xFFFFFFFFu) {
            uart_puts("  [0x"); puts_hex32(off);
            uart_puts("] = "); puts_hex32(v); uart_puts("\n");
        }
    }
}

#endif /* GENET_BASE */
