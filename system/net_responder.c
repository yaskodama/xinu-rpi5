// system/net_responder.c — minimal ARP + ICMP echo responder.
//
// Step 3 of NET-E is fully wiring the xinu-raz network/ stack onto
// GENET via struct netif, but that requires netInit + ethertab +
// ipv4Recv daemon plumbing.  As a stop-gap, we implement just
// enough of an ARP responder and ICMP echo responder right here on
// top of the raw rx_bufs[] from genet, so the user can run `ping`
// from their Mac and see the Pi 4 respond.

#include "uart.h"
#include "genet.h"

/* 32-bit byte-order helpers */
static unsigned short ntohs(unsigned short v) { return (v >> 8) | ((v & 0xFF) << 8); }
static unsigned short htons(unsigned short v) { return ntohs(v); }
static unsigned long  ntohl(unsigned long v)  {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}
static unsigned long  htonl(unsigned long v)  { return ntohl(v); }

/* Pi 4 IP (static for now — DHCP comes in NET-F) */
#define PI_IP_0  192
#define PI_IP_1  168
#define PI_IP_2  3
#define PI_IP_3  101   /* Pi 5 (wired); Pi 4 already uses .100 */

/* GENET sends frames via the existing tx-one-frame path; we'd
 * normally call a dedicated genet_tx(packet, length) helper, but
 * we don't have one yet.  For now we expose this counter so the
 * shell can show how many responses we've sent. */
static unsigned long g_arp_replies;
static unsigned long g_icmp_replies;

unsigned long net_arp_replies(void)  { return g_arp_replies; }
unsigned long net_icmp_replies(void) { return g_icmp_replies; }

/* Forward decl from genet.c: send a raw Ethernet frame.
 * We'll need to add this helper to genet.c separately. */
extern int genet_tx_frame(const unsigned char *frame, int length);

/* Our Pi 4 hardware MAC (set at boot via net_responder_set_mac). */
static unsigned char g_my_mac[6];
static unsigned char g_my_ip[4] = { PI_IP_0, PI_IP_1, PI_IP_2, PI_IP_3 };

void net_responder_set_mac(const unsigned char mac[6])
{
    for (int i = 0; i < 6; i++) g_my_mac[i] = mac[i];
}

/* Override the static IP — called once DHCP binds so ARP/ICMP answer on the
 * leased address instead of the compiled-in fallback. */
void net_responder_set_ip(const unsigned char ip[4])
{
    for (int i = 0; i < 4; i++) g_my_ip[i] = ip[i];
}

void net_responder_get_ip(unsigned char out[4])
{
    for (int i = 0; i < 4; i++) out[i] = g_my_ip[i];
}

/* Send a gratuitous ARP (request with src IP == target IP) so any
 * peer on the broadcast domain can update its ARP cache.  Useful
 * when the router's WiFi-LAN bridge forwards broadcast in one
 * direction only (LAN → WiFi but not the reverse). */
void net_responder_send_gratuitous_arp(void)
{
    uart_puts("garp: G1 enter\n");
    /* `volatile` is critical: with MMU off, all DRAM is Device-
     * nGnRnE on AArch64.  GCC merges adjacent byte stores into
     * stp/strh which fault on Device memory at non-aligned offsets
     * (e.g. stp at offset 12 needs 8-byte alignment).  volatile
     * forces strict 1-byte stores. */
    static volatile unsigned char __attribute__((aligned(16))) frame[60];
    uart_puts("garp: G2 zeroing\n");
    for (int i = 0; i < 60; i++) frame[i] = 0;
    uart_puts("garp: G3 setting dst broadcast\n");
    for (int i = 0; i < 6; i++) frame[i] = 0xFF;
    uart_puts("garp: G4 setting src MAC\n");
    for (int i = 0; i < 6; i++) frame[6 + i] = g_my_mac[i];
    frame[12] = 0x08; frame[13] = 0x06;
    frame[14] = 0x00; frame[15] = 0x01;
    frame[16] = 0x08; frame[17] = 0x00;
    frame[18] = 6;
    frame[19] = 4;
    frame[20] = 0x00; frame[21] = 0x01;
    for (int i = 0; i < 6; i++) frame[22 + i] = g_my_mac[i];
    frame[28] = g_my_ip[0]; frame[29] = g_my_ip[1];
    frame[30] = g_my_ip[2]; frame[31] = g_my_ip[3];
    frame[38] = g_my_ip[0]; frame[39] = g_my_ip[1];
    frame[40] = g_my_ip[2]; frame[41] = g_my_ip[3];
    uart_puts("garp: G5 calling genet_tx_frame\n");
    int rc = genet_tx_frame(frame, 60);
    uart_puts("garp: G6 returned rc=");
    if (rc < 0) uart_putc('-');
    {
        char b[8]; int n = 0; int v = rc < 0 ? -rc : rc;
        if (v == 0) uart_putc('0');
        else { while (v) { b[n++] = (char)('0' + v%10); v /= 10; }
               while (n--) uart_putc(b[n]); }
    }
    uart_puts("\n");
}

/* Outgoing frame buffer.  `volatile` prevents GCC from merging
 * adjacent byte stores into unaligned stp/strh — these trap on
 * Device-nGnRnE DRAM when the MMU is off (see gratuitous-ARP
 * frame[] for the same fix). */
static volatile unsigned char __attribute__((aligned(64))) tx_reply[1518];

static void mac_copy(unsigned char *d, const unsigned char *s)
{
    for (int i = 0; i < 6; i++) d[i] = s[i];
}

static int mac_equal(const unsigned char *a, const unsigned char *b)
{
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void puts_ip(const unsigned char *ip)
{
    char buf[16]; int n = 0;
    for (int oct = 0; oct < 4; oct++) {
        unsigned v = ip[oct];
        if (v >= 100) { buf[n++] = (char)('0' + v / 100); v %= 100; }
        if (v >= 10 || ip[oct] >= 100) { buf[n++] = (char)('0' + v / 10); v %= 10; }
        buf[n++] = (char)('0' + v);
        if (oct < 3) buf[n++] = '.';
    }
    buf[n] = 0;
    uart_puts(buf);
}

/* IP checksum (RFC 1071): one's-complement sum of 16-bit words */
static unsigned short ip_checksum(const unsigned char *data, int len)
{
    unsigned long sum = 0;
    while (len > 1) {
        sum += ((unsigned long)data[0] << 8) | data[1];
        data += 2; len -= 2;
    }
    if (len) sum += (unsigned long)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)(~sum & 0xFFFF);
}

/* Handle one received Ethernet frame.  Return 1 if we responded,
 * 0 if we ignored it. */
int net_responder_handle(const unsigned char *frame, int len)
{
    if (len < 14) return 0;
    unsigned short ethtype = ((unsigned short)frame[12] << 8) | frame[13];

    /* -------- ARP (0x0806) -------- */
    if (ethtype == 0x0806 && len >= 14 + 28) {
        const unsigned char *arp = frame + 14;
        unsigned short oper = ((unsigned short)arp[6] << 8) | arp[7];
        if (oper != 0x0001) return 0;          /* not a request */

        /* Check target IP matches ours */
        const unsigned char *tpa = arp + 24;
        if (tpa[0] != g_my_ip[0] || tpa[1] != g_my_ip[1] ||
            tpa[2] != g_my_ip[2] || tpa[3] != g_my_ip[3]) return 0;

        /* Throttle logging: the framebuffer console is slow, and a
         * uart_puts() per packet lengthens the RX drain enough to back
         * the ring up under load.  Show the first few, then go quiet
         * (use the `rxstat` counters for ongoing volume). */
        static int arp_log = 8;
        if (arp_log > 0) {
            uart_puts("net: ARP request for "); puts_ip(g_my_ip);
            uart_puts(" from "); puts_ip(arp + 14); uart_puts("\n");
            arp_log--;
        }

        /* Build ARP reply (42 bytes, padded to 60) */
        for (int i = 0; i < 60; i++) tx_reply[i] = 0;
        mac_copy(tx_reply, frame + 6);                /* dst = requester */
        mac_copy(tx_reply + 6, g_my_mac);             /* src = us */
        tx_reply[12] = 0x08; tx_reply[13] = 0x06;     /* EtherType ARP */
        /* ARP body */
        tx_reply[14] = 0x00; tx_reply[15] = 0x01;     /* HTYPE Ethernet */
        tx_reply[16] = 0x08; tx_reply[17] = 0x00;     /* PTYPE IPv4 */
        tx_reply[18] = 6;
        tx_reply[19] = 4;
        tx_reply[20] = 0x00; tx_reply[21] = 0x02;     /* OPER = reply */
        mac_copy(tx_reply + 22, g_my_mac);            /* SHA = us */
        tx_reply[28] = g_my_ip[0];                    /* SPA */
        tx_reply[29] = g_my_ip[1];
        tx_reply[30] = g_my_ip[2];
        tx_reply[31] = g_my_ip[3];
        mac_copy(tx_reply + 32, frame + 6);           /* THA */
        tx_reply[38] = arp[14];                       /* TPA */
        tx_reply[39] = arp[15];
        tx_reply[40] = arp[16];
        tx_reply[41] = arp[17];

        genet_tx_frame(tx_reply, 60);
        g_arp_replies++;
        return 1;
    }

    /* -------- IPv4 (0x0800) -------- */
    if (ethtype == 0x0800 && len >= 14 + 20) {
        const unsigned char *ip = frame + 14;
        int ihl = (ip[0] & 0x0F) * 4;
        if (ihl < 20) return 0;
        unsigned char proto = ip[9];
        const unsigned char *src_ip = ip + 12;
        const unsigned char *dst_ip = ip + 16;
        if (dst_ip[0] != g_my_ip[0] || dst_ip[1] != g_my_ip[1] ||
            dst_ip[2] != g_my_ip[2] || dst_ip[3] != g_my_ip[3]) return 0;

        /* ICMP (proto = 1) */
        if (proto == 1 && len >= 14 + ihl + 8) {
            const unsigned char *icmp = ip + ihl;
            unsigned char icmp_type = icmp[0];
            if (icmp_type != 8) return 0;             /* echo request */

            int total_len = ((unsigned short)ip[2] << 8) | ip[3];
            int icmp_len = total_len - ihl;
            if (icmp_len < 8 || total_len + 14 > 1518) return 0;

            /* Throttle (see ARP path): print the first few echoes so
             * the operator can confirm ping reached us, then stay
             * silent so the hot RX path is fast under sustained load. */
            static int icmp_log = 8;
            if (icmp_log > 0) {
                uart_puts("net: ICMP echo from "); puts_ip(src_ip);
                uart_puts(" len="); {
                    char b[8]; int n = 0; int v = icmp_len;
                    if (v == 0) uart_putc('0');
                    else { while (v) { b[n++] = (char)('0' + v%10); v /= 10; }
                           while (n--) uart_putc(b[n]); }
                }
                uart_puts("\n");
                icmp_log--;
            }

            /* Build ICMP echo reply: copy entire request, swap MACs,
             * swap IPs, set type=0, recompute checksums. */
            int reply_len = 14 + total_len;
            for (int i = 0; i < reply_len; i++) tx_reply[i] = frame[i];

            mac_copy(tx_reply, frame + 6);            /* dst = sender */
            mac_copy(tx_reply + 6, g_my_mac);         /* src = us */

            unsigned char *r_ip = tx_reply + 14;
            /* swap IP src/dst */
            for (int i = 0; i < 4; i++) {
                r_ip[12 + i] = g_my_ip[i];
                r_ip[16 + i] = src_ip[i];
            }
            /* zero TTL high half? actually keep TTL. */
            /* IP checksum: zero, then recompute */
            r_ip[10] = 0; r_ip[11] = 0;
            unsigned short ipsum = ip_checksum(r_ip, ihl);
            r_ip[10] = (unsigned char)(ipsum >> 8);
            r_ip[11] = (unsigned char)(ipsum & 0xFF);

            /* ICMP: type=0 (reply), recompute checksum */
            unsigned char *r_icmp = r_ip + ihl;
            r_icmp[0] = 0;                            /* echo reply */
            r_icmp[2] = 0; r_icmp[3] = 0;
            unsigned short icmpsum = ip_checksum(r_icmp, icmp_len);
            r_icmp[2] = (unsigned char)(icmpsum >> 8);
            r_icmp[3] = (unsigned char)(icmpsum & 0xFF);

            genet_tx_frame(tx_reply, reply_len < 60 ? 60 : reply_len);
            g_icmp_replies++;
            return 1;
        }
    }

    return 0;
}
