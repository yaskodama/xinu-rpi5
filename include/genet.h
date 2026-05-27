// include/genet.h — BCM2711 GENET (built-in Gigabit Ethernet) driver.
//
// Unlike Pi 3 (LAN9514 USB-attached Ethernet) or QEMU virt (smc91c111),
// Pi 4 has Ethernet built into the BCM2711 SoC via a Broadcom IP block
// called GENET, wired to an external Gigabit PHY (BCM54213PE) on the
// RJ-45 jack.  Linux: drivers/net/ethernet/broadcom/genet/bcmgenet.c.
//
// Phase plan:
//   NET-A  MMIO probe — SYS_REV_CTRL / SYS_PORT_CTRL
//   NET-B  UMAC reset + MAC address from firmware
//   NET-C  TX descriptor ring + raw Ethernet send
//   NET-D  RX descriptor ring + IRQ + raw Ethernet receive
//   NET-E  xinu-raz network/{arp,ipv4,icmp} port -> ping
//   NET-F  DHCP client -> IP address
//   NET-G  TCP + telnetd -> remote shell

#ifndef XINU_RPI5_GENET_H
#define XINU_RPI5_GENET_H

#ifdef GENET_BASE   /* Pi 4 only — PI4_CFLAGS supplies the value */

void genet_init(void);

/* Phase NET-A diagnostics, exposed for the shell `genet` command. */
unsigned int genet_sys_rev(void);
unsigned int genet_sys_port_ctrl(void);

/* Phase NET-B — UMAC reset + MAC address.  Fills the 6-byte array
 * with the MAC pulled out of UMAC_MAC0 / UMAC_MAC1 (which firmware
 * has already populated from OTP / VC). */
void genet_get_mac(unsigned char mac[6]);

/* Phase NET-D — poll RX path for one frame.  Returns the byte
 * length of the received frame and writes a pointer to the
 * buffer into *out_pkt, or 0 if no frame is pending. */
int  genet_rx_poll(unsigned char **out_pkt);

/* Release the buffer returned by genet_rx_poll() so HW can reuse it. */
void genet_rx_release(void);

/* Recover a wedged / overrun RX path (RBUF flush + ring re-arm +
 * consumer-index resync).  Called automatically from genet_rx_poll()
 * on overrun; exported so the shell can force it for diagnosis. */
void genet_rx_recover(void);

/* Statistics for the shell `rxstat` / `net` commands. */
unsigned long genet_rx_packet_count(void);
unsigned long genet_rx_byte_count(void);
unsigned long genet_rx_overrun_count(void);
unsigned long genet_rx_recover_count(void);
unsigned long genet_tx_timeout_count(void);

/* Step 3 — send an arbitrary Ethernet frame (buf, len) on TDMA ring 16.
 * Returns 0 on success, -1 on TX timeout / not initialised. */
int genet_tx_frame(const unsigned char *frame, int length);

/* Re-read BMSR and report link status — used by main.c to show
 * the link state at boot end (after the shell ring scrolls past
 * the original PHY-init log). */
int  genet_link_up(void);
unsigned int genet_phy_bmsr(void);

#else  /* GENET_BASE not defined (Pi 5 / QEMU) — no built-in GENET MAC */

/* main.c, shell.c and the RX dispatcher reference these unconditionally
 * (the calls are not wrapped in #ifdef GENET_BASE), so provide inert
 * stubs for every non-pi4 variant.  Keeping them here is what lets the
 * pi5 and qemu targets compile cleanly. */
static inline void          genet_init(void)                 {}
static inline unsigned int  genet_sys_rev(void)              { return 0; }
static inline unsigned int  genet_sys_port_ctrl(void)        { return 0; }
static inline void          genet_get_mac(unsigned char m[6]){ (void)m; }
static inline int           genet_rx_poll(unsigned char **p) { (void)p; return 0; }
static inline void          genet_rx_release(void)           {}
static inline void          genet_rx_recover(void)           {}
static inline unsigned long genet_rx_packet_count(void)      { return 0; }
static inline unsigned long genet_rx_byte_count(void)        { return 0; }
static inline unsigned long genet_rx_overrun_count(void)     { return 0; }
static inline unsigned long genet_rx_recover_count(void)     { return 0; }
static inline unsigned long genet_tx_timeout_count(void)     { return 0; }
static inline int           genet_tx_frame(const unsigned char *f, int n)
                                                             { (void)f; (void)n; return -1; }
static inline int           genet_link_up(void)              { return 0; }
static inline unsigned int  genet_phy_bmsr(void)             { return 0xFFFFFFFFu; }

#endif

#endif /* XINU_RPI5_GENET_H */
