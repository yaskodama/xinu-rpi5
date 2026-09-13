// system/dhcp_client.c — minimal DHCP client (NET-F).
//
// State machine: INIT → DISCOVER → SELECTING → REQUEST → BOUND.
// On BOUND the lease is logged to UART; the rest of the responder
// (net_responder.c) keeps its hard-coded fallback IP for now, but the
// `dhcp_get_ip()` accessor lets later code rebind.
//
// Frame buffer is volatile + aligned(64) — same Device-nGnRnE fix
// the rest of the GENET path uses (otherwise GCC merges adjacent
// byte stores into stp and faults on MMIO-mapped buffers when MMU
// is off).

#include "uart.h"
#include "genet.h"

extern int genet_tx_frame(const unsigned char *frame, int length);

/* ---- byte-order helpers (private, same logic as net_responder) ---- */
static unsigned short hton16(unsigned short v) { return (unsigned short)((v >> 8) | ((v & 0xFF) << 8)); }
static unsigned long  hton32(unsigned long v)  {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}

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

/* UDP checksum over IP pseudo-header + UDP header + payload.  The
 * `udp` buffer must already contain UDP header (with checksum field
 * zeroed) and full payload.  Length covers UDP header + payload. */
static unsigned short udp_checksum(const unsigned char *src_ip,
                                   const unsigned char *dst_ip,
                                   const unsigned char *udp, int udp_len)
{
    unsigned long sum = 0;
    /* pseudo-header: src IP, dst IP */
    for (int i = 0; i < 4; i += 2)
        sum += ((unsigned long)src_ip[i] << 8) | src_ip[i + 1];
    for (int i = 0; i < 4; i += 2)
        sum += ((unsigned long)dst_ip[i] << 8) | dst_ip[i + 1];
    /* pseudo-header: zero | protocol(17) | UDP length */
    sum += 17;
    sum += udp_len;
    /* UDP header + payload */
    int i = 0;
    while (i + 1 < udp_len) {
        sum += ((unsigned long)udp[i] << 8) | udp[i + 1];
        i += 2;
    }
    if (i < udp_len) sum += (unsigned long)udp[i] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    unsigned short cs = (unsigned short)(~sum & 0xFFFF);
    return cs ? cs : 0xFFFF;     /* RFC 768: 0 means no checksum */
}

/* ---- DHCP state ---- */
enum { DHCP_INIT = 0, DHCP_DISCOVER_SENT, DHCP_REQUEST_SENT, DHCP_BOUND };
static unsigned char g_state = DHCP_INIT;
static unsigned char g_my_mac[6];
static unsigned char g_my_ip[4];        /* 0.0.0.0 until BOUND */
static unsigned char g_server_ip[4];    /* DHCP server (set from OFFER opt 54) */
static unsigned char g_router_ip[4];
static unsigned char g_netmask[4];
static unsigned char g_dns_ip[4];
static unsigned long g_lease_secs;
static unsigned long g_xid = 0xC0FFEE00ul;
static unsigned long g_discover_count;
static unsigned long g_offer_count;
static unsigned long g_request_count;
static unsigned long g_ack_count;

void dhcp_set_mac(const unsigned char mac[6])
{
    for (int i = 0; i < 6; i++) g_my_mac[i] = mac[i];
    /* Make xid unique-ish per boot: MAC tail XORed into base. */
    g_xid ^= ((unsigned long)mac[3] << 16)
           | ((unsigned long)mac[4] << 8)
           |  (unsigned long)mac[5];
}

int dhcp_is_bound(void) { return g_state == DHCP_BOUND; }
unsigned char dhcp_state(void) { return g_state; }
void dhcp_get_ip(unsigned char out[4])      { for (int i = 0; i < 4; i++) out[i] = g_my_ip[i]; }
void dhcp_get_router(unsigned char out[4])  { for (int i = 0; i < 4; i++) out[i] = g_router_ip[i]; }
void dhcp_get_netmask(unsigned char out[4]) { for (int i = 0; i < 4; i++) out[i] = g_netmask[i]; }
/* ブラウザが名前を引くのに要る。DHCP は受け取っていたが、外へ出す口が無かった。 */
void dhcp_get_dns(unsigned char out[4])     { for (int i = 0; i < 4; i++) out[i] = g_dns_ip[i]; }
unsigned long dhcp_lease_seconds(void)      { return g_lease_secs; }
unsigned long dhcp_discover_count(void)     { return g_discover_count; }
unsigned long dhcp_offer_count(void)        { return g_offer_count; }
unsigned long dhcp_request_count(void)      { return g_request_count; }
unsigned long dhcp_ack_count(void)          { return g_ack_count; }

/* ---- frame builder ----
 *
 * Output layout in tx_frame[]:
 *   0..13   Ethernet header  (dst broadcast, src=my_mac, type=0x0800)
 *  14..33   IPv4 header      (20 bytes, no options)
 *  34..41   UDP header       (src=68, dst=67, len, checksum)
 *  42..    BOOTP/DHCP        (236 fixed + cookie + options)
 *
 * BOOTP fixed payload is 236 bytes, plus 4 magic, plus options.  Total
 * UDP payload size is therefore at least 240 + options bytes.        */
static volatile unsigned char __attribute__((aligned(64))) tx_frame[600];

static volatile unsigned char *put_option_byte(volatile unsigned char *p,
                                               unsigned char opt, unsigned char v)
{
    *p++ = opt; *p++ = 1; *p++ = v;
    return p;
}

static volatile unsigned char *put_option_data(volatile unsigned char *p, unsigned char opt,
                                               const unsigned char *src, int len)
{
    *p++ = opt; *p++ = (unsigned char)len;
    for (int i = 0; i < len; i++) *p++ = src[i];
    return p;
}

/* Build a DHCP message. Returns total frame length (Ethernet onwards). */
static int build_dhcp(int msg_type,
                      const unsigned char *requested_ip,   /* nullable */
                      const unsigned char *server_ip)      /* nullable */
{
    /* Zero the workspace up to a comfortable upper bound. */
    for (int i = 0; i < 400; i++) tx_frame[i] = 0;

    /* --- Ethernet --- */
    for (int i = 0; i < 6; i++) tx_frame[i] = 0xFF;          /* dst = broadcast */
    for (int i = 0; i < 6; i++) tx_frame[6 + i] = g_my_mac[i];
    tx_frame[12] = 0x08; tx_frame[13] = 0x00;                /* EtherType IPv4 */

    /* --- DHCP / BOOTP fixed part at offset 42 --- */
    volatile unsigned char *bp = tx_frame + 42;
    bp[0] = 1;       /* op = BOOTREQUEST */
    bp[1] = 1;       /* htype = Ethernet */
    bp[2] = 6;       /* hlen = 6 */
    bp[3] = 0;       /* hops */
    bp[4] = (unsigned char)(g_xid >> 24);
    bp[5] = (unsigned char)(g_xid >> 16);
    bp[6] = (unsigned char)(g_xid >> 8);
    bp[7] = (unsigned char)(g_xid);
    bp[8] = 0; bp[9] = 0;          /* secs */
    bp[10] = 0x80; bp[11] = 0x00;  /* flags = broadcast */
    /* ciaddr, yiaddr, siaddr, giaddr already 0 (memset above) */
    for (int i = 0; i < 6; i++) bp[28 + i] = g_my_mac[i];     /* chaddr */
    /* sname (64) and file (128) already zero. */
    /* Magic cookie at offset 236 of BOOTP payload */
    bp[236] = 0x63; bp[237] = 0x82; bp[238] = 0x53; bp[239] = 0x63;

    /* --- DHCP options --- */
    volatile unsigned char *opt = bp + 240;
    opt = put_option_byte(opt, 53, (unsigned char)msg_type);   /* DHCP message type */
    /* Client identifier: hardware type (1) + MAC (6) */
    {
        unsigned char cid[7];
        cid[0] = 1;
        for (int i = 0; i < 6; i++) cid[1 + i] = g_my_mac[i];
        opt = put_option_data(opt, 61, cid, 7);
    }
    if (requested_ip) {
        opt = put_option_data(opt, 50, requested_ip, 4);       /* requested IP */
    }
    if (server_ip) {
        opt = put_option_data(opt, 54, server_ip, 4);          /* server id */
    }
    {
        unsigned char params[5] = { 1, 3, 6, 15, 51 };
        /* subnet mask, router, DNS, domain name, lease time */
        opt = put_option_data(opt, 55, params, 5);
    }
    *opt++ = 255;   /* end */

    int dhcp_len  = (int)(opt - bp);
    int udp_len   = 8 + dhcp_len;
    int ip_total  = 20 + udp_len;

    /* --- UDP header at offset 34 --- */
    volatile unsigned char *uh = tx_frame + 34;
    uh[0] = 0; uh[1] = 68;                                     /* src port 68 (client) */
    uh[2] = 0; uh[3] = 67;                                     /* dst port 67 (server) */
    uh[4] = (unsigned char)(udp_len >> 8);
    uh[5] = (unsigned char)(udp_len & 0xFF);
    uh[6] = 0; uh[7] = 0;                                      /* checksum filled below */

    /* --- IPv4 header at offset 14 --- */
    volatile unsigned char *ih = tx_frame + 14;
    ih[0]  = 0x45;                                             /* IPv4, IHL=5 */
    ih[1]  = 0;                                                /* DSCP/ECN */
    ih[2]  = (unsigned char)(ip_total >> 8);
    ih[3]  = (unsigned char)(ip_total & 0xFF);
    ih[4]  = 0; ih[5] = 0;                                     /* identification */
    ih[6]  = 0; ih[7] = 0;                                     /* flags / frag */
    ih[8]  = 64;                                               /* TTL */
    ih[9]  = 17;                                               /* protocol = UDP */
    ih[10] = 0; ih[11] = 0;                                    /* IP checksum (filled below) */
    /* src IP = 0.0.0.0 (already zero) */
    ih[16] = 255; ih[17] = 255; ih[18] = 255; ih[19] = 255;    /* dst IP = broadcast */

    unsigned short ipsum = ip_checksum((const unsigned char *)ih, 20);
    ih[10] = (unsigned char)(ipsum >> 8);
    ih[11] = (unsigned char)(ipsum & 0xFF);

    /* UDP checksum over pseudo-header + UDP header + payload */
    unsigned char zero_ip[4] = {0, 0, 0, 0};
    unsigned char bcst_ip[4] = {255, 255, 255, 255};
    /* Need a const-ish view of uh..uh+udp_len; cast away volatile for ckusm */
    unsigned short ucs = udp_checksum(zero_ip, bcst_ip,
                                      (const unsigned char *)uh, udp_len);
    uh[6] = (unsigned char)(ucs >> 8);
    uh[7] = (unsigned char)(ucs & 0xFF);

    return 14 + ip_total;
}

/* Convenience puts_ip — copy here so we don't have to expose
 * net_responder's helper across compilation units. */
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

static void puts_u32_dec(unsigned long v)
{
    char b[12]; int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) uart_putc(b[n]);
}

void dhcp_send_discover(void)
{
    g_discover_count++;
    int len = build_dhcp(1 /* DISCOVER */, 0, 0);
    int rc = genet_tx_frame((const unsigned char *)tx_frame, len);
    uart_puts("dhcp: DISCOVER sent len=");
    puts_u32_dec((unsigned long)len);
    uart_puts(" rc=");
    if (rc < 0) { uart_putc('-'); rc = -rc; }
    puts_u32_dec((unsigned long)rc);
    uart_puts("\n");
    if (g_state == DHCP_INIT) g_state = DHCP_DISCOVER_SENT;
}

static void dhcp_send_request(void)
{
    g_request_count++;
    int len = build_dhcp(3 /* REQUEST */, g_my_ip, g_server_ip);
    uart_puts("dhcp: send REQUEST for "); puts_ip(g_my_ip);
    uart_puts(" via "); puts_ip(g_server_ip); uart_puts("\n");
    genet_tx_frame((const unsigned char *)tx_frame, len);
    g_state = DHCP_REQUEST_SENT;
}

/* Parse a received DHCP message (frame starts at Ethernet header).
 * Returns 1 if it was a DHCP packet we processed, 0 otherwise.  */
int dhcp_handle_packet(const unsigned char *frame, int len)
{
    if (len < 14 + 20 + 8 + 240 + 4) return 0;
    if (frame[12] != 0x08 || frame[13] != 0x00) return 0;     /* not IPv4 */
    const unsigned char *ip = frame + 14;
    if ((ip[0] >> 4) != 4) return 0;                           /* not IPv4 */
    int ihl = (ip[0] & 0x0F) * 4;
    if (ip[9] != 17) return 0;                                 /* not UDP */
    const unsigned char *udp = ip + ihl;
    unsigned short sport = ((unsigned short)udp[0] << 8) | udp[1];
    unsigned short dport = ((unsigned short)udp[2] << 8) | udp[3];
    if (sport != 67 || dport != 68) return 0;                  /* not DHCP server→client */

    const unsigned char *bp = udp + 8;
    int bp_len = len - (int)(bp - frame);
    if (bp_len < 240) return 0;

    /* Match xid */
    unsigned long got_xid =
        ((unsigned long)bp[4] << 24) | ((unsigned long)bp[5] << 16) |
        ((unsigned long)bp[6] << 8)  |  (unsigned long)bp[7];
    if (got_xid != g_xid) return 0;

    /* Magic cookie check */
    if (bp[236] != 0x63 || bp[237] != 0x82 || bp[238] != 0x53 || bp[239] != 0x63)
        return 0;

    /* yiaddr at offset 16 of BOOTP */
    unsigned char yiaddr[4] = { bp[16], bp[17], bp[18], bp[19] };

    /* Parse options */
    int msg_type = 0;
    unsigned char server_id[4] = {0, 0, 0, 0};
    unsigned char router[4]    = {0, 0, 0, 0};
    unsigned char netmask[4]   = {0, 0, 0, 0};
    unsigned char dns[4]       = {0, 0, 0, 0};
    unsigned long lease        = 0;

    int o = 240;
    while (o < bp_len) {
        unsigned char code = bp[o++];
        if (code == 0)   continue;                              /* pad */
        if (code == 255) break;                                 /* end */
        if (o >= bp_len) break;
        int olen = bp[o++];
        if (o + olen > bp_len) break;
        const unsigned char *od = bp + o;
        switch (code) {
        case 53: if (olen >= 1) msg_type = od[0]; break;
        case 54: if (olen >= 4) for (int i = 0; i < 4; i++) server_id[i] = od[i]; break;
        case 3:  if (olen >= 4) for (int i = 0; i < 4; i++) router[i]    = od[i]; break;
        case 1:  if (olen >= 4) for (int i = 0; i < 4; i++) netmask[i]   = od[i]; break;
        case 6:  if (olen >= 4) for (int i = 0; i < 4; i++) dns[i]       = od[i]; break;
        case 51: if (olen >= 4) lease = ((unsigned long)od[0] << 24) |
                                        ((unsigned long)od[1] << 16) |
                                        ((unsigned long)od[2] << 8)  |
                                         (unsigned long)od[3]; break;
        default: break;
        }
        o += olen;
    }

    if (msg_type == 2 /* OFFER */ && g_state == DHCP_DISCOVER_SENT) {
        g_offer_count++;
        for (int i = 0; i < 4; i++) g_my_ip[i]     = yiaddr[i];
        for (int i = 0; i < 4; i++) g_server_ip[i] = server_id[i];
        for (int i = 0; i < 4; i++) g_router_ip[i] = router[i];
        for (int i = 0; i < 4; i++) g_netmask[i]   = netmask[i];
        for (int i = 0; i < 4; i++) g_dns_ip[i]    = dns[i];
        g_lease_secs = lease;
        uart_puts("dhcp: OFFER "); puts_ip(yiaddr);
        uart_puts(" from server "); puts_ip(server_id); uart_puts("\n");
        dhcp_send_request();
        return 1;
    }

    if (msg_type == 5 /* ACK */ && g_state == DHCP_REQUEST_SENT) {
        g_ack_count++;
        for (int i = 0; i < 4; i++) g_my_ip[i] = yiaddr[i];
        g_state = DHCP_BOUND;
        uart_puts("dhcp: BOUND ip="); puts_ip(yiaddr);
        uart_puts(" mask="); puts_ip(netmask);
        uart_puts(" gw="); puts_ip(router);
        uart_puts(" lease="); puts_u32_dec(lease);
        uart_puts("s\n");
        return 1;
    }

    if (msg_type == 6 /* NAK */) {
        uart_puts("dhcp: NAK — resetting to INIT\n");
        g_state = DHCP_INIT;
        return 1;
    }

    return 0;
}
