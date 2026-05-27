// system/tcp_server.c — minimal TCP listener (NET-G Step 1).
//
// Scope: handle exactly one simultaneous TCP connection on a fixed
// local port (default 23 / telnet).  No retransmission, no out-of-order
// reassembly — a peer that drops a segment will hang the connection.
// Enough to demonstrate that Pi 4 can complete a 3-way handshake,
// echo a greeting, accept a few keystrokes, then close cleanly.
//
// Layout in tx_frame[]:
//   0..13   Ethernet  (dst=peer mac, src=my mac, type=0x0800)
//  14..33   IPv4      (20 bytes, src=my ip, dst=peer ip, proto=6)
//  34..53   TCP       (20 bytes, no options)
//  54..    payload    (e.g. greeting bytes)

#include "uart.h"
#include "genet.h"

extern int genet_tx_frame(const unsigned char *frame, int length);

/* --- byte/checksum helpers (duplicated, intentionally — keeps
 *     this file standalone of net_responder/dhcp_client) --- */
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

static unsigned short tcp_checksum(const unsigned char *src_ip,
                                   const unsigned char *dst_ip,
                                   const unsigned char *tcp, int tcp_len)
{
    unsigned long sum = 0;
    for (int i = 0; i < 4; i += 2)
        sum += ((unsigned long)src_ip[i] << 8) | src_ip[i + 1];
    for (int i = 0; i < 4; i += 2)
        sum += ((unsigned long)dst_ip[i] << 8) | dst_ip[i + 1];
    sum += 6;             /* protocol = TCP */
    sum += tcp_len;
    int i = 0;
    while (i + 1 < tcp_len) {
        sum += ((unsigned long)tcp[i] << 8) | tcp[i + 1];
        i += 2;
    }
    if (i < tcp_len) sum += (unsigned long)tcp[i] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)(~sum & 0xFFFF);
}

/* --- TCP state machine (RFC 793, subset) --- */
enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_LAST_ACK,
};

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

struct tcp_conn {
    int state;
    unsigned char peer_mac[6];
    unsigned char peer_ip[4];
    unsigned short peer_port;
    unsigned long my_seq;       /* next byte we will send */
    unsigned long peer_seq;     /* next byte we expect to receive */
    int greeted;
};

static struct tcp_conn g_conn;
static unsigned char g_my_mac[6];
static unsigned char g_my_ip[4]  = { 192, 168, 3, 100 };
static unsigned short g_listen_port = 23;
static unsigned long g_isn_seed = 0xDEADBEEF;

/* Diagnostic counters — surfaced by the shell `tcpstat` command so we
 * can see what happened without relying on scrolled-off boot output. */
static unsigned long g_tcp_any;       /* TCP frames to our IP, any port      */
static unsigned long g_seg_rx;        /* TCP segments addressed to our port */
static unsigned long g_syn_seen;      /* SYNs that opened a connection       */
static unsigned long g_synack_sent;   /* SYN+ACKs we transmitted             */
static unsigned long g_estab;         /* transitions to ESTABLISHED          */
static unsigned long g_txfail;        /* tcp_send() that hit a TX error       */
static unsigned char  g_last_ip[4];   /* most recent peer (even if rejected) */
static unsigned short g_last_port;

/* Outgoing frame buffer — volatile + aligned(64), see net_responder
 * for the rationale (MMU off + Device-nGnRnE + GCC store merging). */
static volatile unsigned char __attribute__((aligned(64))) tx_frame[1518];

void tcp_set_mac(const unsigned char mac[6])
{
    for (int i = 0; i < 6; i++) g_my_mac[i] = mac[i];
}

void tcp_set_ip(const unsigned char ip[4])
{
    for (int i = 0; i < 4; i++) g_my_ip[i] = ip[i];
}

void tcp_listen(unsigned short port)
{
    g_listen_port = port;
    g_conn.state = TCP_LISTEN;
    g_conn.greeted = 0;
}

static void puts_u32_dec(unsigned long v)
{
    char b[12]; int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) uart_putc(b[n]);
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

/* Build and send a single TCP segment.  `payload` may be NULL (header
 * only).  Returns 0 on success, -1 on failure. */
static int tcp_send(unsigned char flags, const char *payload, int payload_len)
{
    if (payload_len < 0) payload_len = 0;
    int tcp_len = 20 + payload_len;
    int ip_total = 20 + tcp_len;
    int frame_len = 14 + ip_total;
    if (frame_len > 1518) return -1;

    /* zero header span */
    for (int i = 0; i < 54; i++) tx_frame[i] = 0;

    /* --- Ethernet --- */
    for (int i = 0; i < 6; i++) tx_frame[i]     = g_conn.peer_mac[i];
    for (int i = 0; i < 6; i++) tx_frame[6 + i] = g_my_mac[i];
    tx_frame[12] = 0x08; tx_frame[13] = 0x00;

    /* --- IPv4 --- */
    volatile unsigned char *ih = tx_frame + 14;
    ih[0]  = 0x45;
    ih[1]  = 0;
    ih[2]  = (unsigned char)(ip_total >> 8);
    ih[3]  = (unsigned char)(ip_total & 0xFF);
    ih[4]  = 0; ih[5] = 0;
    ih[6]  = 0; ih[7] = 0;
    ih[8]  = 64;
    ih[9]  = 6;                                /* proto = TCP */
    ih[10] = 0; ih[11] = 0;
    for (int i = 0; i < 4; i++) ih[12 + i] = g_my_ip[i];
    for (int i = 0; i < 4; i++) ih[16 + i] = g_conn.peer_ip[i];
    unsigned short ipsum = ip_checksum((const unsigned char *)ih, 20);
    ih[10] = (unsigned char)(ipsum >> 8);
    ih[11] = (unsigned char)(ipsum & 0xFF);

    /* --- TCP --- */
    volatile unsigned char *th = tx_frame + 34;
    th[0] = (unsigned char)(g_listen_port >> 8);
    th[1] = (unsigned char)(g_listen_port & 0xFF);
    th[2] = (unsigned char)(g_conn.peer_port >> 8);
    th[3] = (unsigned char)(g_conn.peer_port & 0xFF);
    th[4] = (unsigned char)(g_conn.my_seq >> 24);
    th[5] = (unsigned char)(g_conn.my_seq >> 16);
    th[6] = (unsigned char)(g_conn.my_seq >> 8);
    th[7] = (unsigned char)(g_conn.my_seq);
    th[8]  = (unsigned char)(g_conn.peer_seq >> 24);
    th[9]  = (unsigned char)(g_conn.peer_seq >> 16);
    th[10] = (unsigned char)(g_conn.peer_seq >> 8);
    th[11] = (unsigned char)(g_conn.peer_seq);
    th[12] = 0x50;                             /* data offset = 5 (20 bytes) */
    th[13] = flags;
    th[14] = 0x20; th[15] = 0x00;              /* window = 8192 */
    th[16] = 0; th[17] = 0;                    /* checksum (filled below) */
    th[18] = 0; th[19] = 0;                    /* urgent */

    if (payload && payload_len > 0) {
        for (int i = 0; i < payload_len; i++)
            tx_frame[54 + i] = (unsigned char)payload[i];
    }

    unsigned short ucs = tcp_checksum(g_my_ip, g_conn.peer_ip,
                                      (const unsigned char *)th, tcp_len);
    th[16] = (unsigned char)(ucs >> 8);
    th[17] = (unsigned char)(ucs & 0xFF);

    /* Pad to the 60-byte Ethernet minimum (the responder does the same
     * for ARP/ICMP).  A header-only SYN+ACK / ACK / FIN is only 54
     * bytes on the wire — a runt that the switch or the peer NIC will
     * silently drop, which is why ping (always >= 60 B) worked but the
     * TCP handshake never completed.  The extra bytes sit past the IP
     * total-length field, so they are ignored as Ethernet padding. */
    int send_len = frame_len;
    if (send_len < 60) {
        for (int i = frame_len; i < 60; i++) tx_frame[i] = 0;
        send_len = 60;
    }

    return genet_tx_frame((const unsigned char *)tx_frame, send_len);
}

/* Handle one received Ethernet frame.  Returns 1 if it was TCP-for-us
 * (consumed), 0 otherwise. */
int tcp_handle_packet(const unsigned char *frame, int len)
{
    if (len < 14 + 20 + 20) return 0;
    if (frame[12] != 0x08 || frame[13] != 0x00) return 0;
    /* volatile is CRITICAL here.  The MMU is off, so all DRAM (incl. the
     * GENET rx_bufs[] this points into) is Device-nGnRnE on AArch64.
     * Without volatile, GCC -O2 recognises the big-endian byte-OR idiom
     * below (seq/ack) and folds it into a single 32-bit LDR — an
     * UNALIGNED word load (the TCP seq sits at frame offset 38, only
     * 2-byte aligned), which data-aborts on Device memory and hard-hangs
     * the box.  This is exactly why ICMP/ping (no unaligned wide reads)
     * worked while the first TCP segment froze everything.  volatile
     * forces strict per-byte loads.  Same hazard as the TX-frame stores
     * in net_responder. */
    const volatile unsigned char *ip = frame + 14;
    if ((ip[0] >> 4) != 4) return 0;
    int ihl = (ip[0] & 0x0F) * 4;
    if (ip[9] != 6) return 0;                       /* not TCP */

    if (ip[16] != g_my_ip[0] || ip[17] != g_my_ip[1] ||
        ip[18] != g_my_ip[2] || ip[19] != g_my_ip[3]) return 0;

    const volatile unsigned char *tcp = ip + ihl;
    unsigned short sport = ((unsigned short)tcp[0] << 8) | tcp[1];
    unsigned short dport = ((unsigned short)tcp[2] << 8) | tcp[3];

    /* TCP addressed to *our IP* on ANY port — proves a TCP frame
     * physically reached us (vs. being blocked/lost on the path).
     * Capture the peer here too, even on a port mismatch.  If `any`
     * stays 0 while ICMP works, the SYN never arrived (path/firewall);
     * if `any` climbs but `seg` stays 0, it hit the wrong port. */
    g_tcp_any++;
    for (int i = 0; i < 4; i++) g_last_ip[i] = ip[12 + i];
    g_last_port = sport;

    if (dport != g_listen_port) return 0;
    g_seg_rx++;            /* ...and specifically to our listen port */

    unsigned long seq = ((unsigned long)tcp[4]  << 24) |
                        ((unsigned long)tcp[5]  << 16) |
                        ((unsigned long)tcp[6]  << 8)  |
                         (unsigned long)tcp[7];
    unsigned long ack = ((unsigned long)tcp[8]  << 24) |
                        ((unsigned long)tcp[9]  << 16) |
                        ((unsigned long)tcp[10] << 8)  |
                         (unsigned long)tcp[11];
    unsigned char data_off = (tcp[12] >> 4) * 4;
    unsigned char flags    = tcp[13] & 0x3F;
    int total_len = ((unsigned short)ip[2] << 8) | ip[3];
    int data_len  = total_len - ihl - data_off;
    const volatile unsigned char *data = tcp + data_off;

    /* LISTEN: only SYN starts a connection.  Anything else gets RST. */
    if (g_conn.state == TCP_CLOSED || g_conn.state == TCP_LISTEN) {
        if (!(flags & TCP_FLAG_SYN)) return 1;       /* ignore */

        /* Capture peer */
        for (int i = 0; i < 6; i++) g_conn.peer_mac[i] = frame[6 + i];
        for (int i = 0; i < 4; i++) g_conn.peer_ip[i]  = ip[12 + i];
        g_conn.peer_port = sport;
        g_conn.peer_seq  = seq + 1;                  /* ack the SYN */
        g_conn.my_seq    = g_isn_seed;
        g_isn_seed      += 0x100;
        g_conn.greeted   = 0;

        uart_puts("tcp: SYN from "); puts_ip(g_conn.peer_ip);
        uart_puts(":"); puts_u32_dec(sport); uart_puts(" -> SYN+ACK\n");

        /* Send SYN+ACK.  SYN occupies one sequence number, so the
         * next byte we send (the greeting) starts at my_seq + 1. */
        g_syn_seen++;
        if (tcp_send(TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0) < 0) g_txfail++;
        else                                                 g_synack_sent++;
        g_conn.my_seq += 1;
        g_conn.state   = TCP_SYN_RCVD;
        return 1;
    }

    /* Match the active connection 4-tuple */
    if (g_conn.peer_port != sport) return 1;
    for (int i = 0; i < 4; i++)
        if (g_conn.peer_ip[i] != ip[12 + i]) return 1;

    if (g_conn.state == TCP_SYN_RCVD) {
        if (flags & TCP_FLAG_RST) { g_conn.state = TCP_LISTEN; return 1; }
        if ((flags & TCP_FLAG_ACK) && ack == g_conn.my_seq) {
            g_conn.state = TCP_ESTABLISHED;
            g_estab++;
            uart_puts("tcp: ESTABLISHED\n");
            if (!g_conn.greeted) {
                static const char hello[] =
                    "Hello from xinu-rpi5 (Pi 4, BCM2711)!\r\n"
                    "Type ENTER to close.\r\n";
                int n = (int)(sizeof(hello) - 1);
                tcp_send(TCP_FLAG_PSH | TCP_FLAG_ACK, hello, n);
                g_conn.my_seq += n;
                g_conn.greeted = 1;
            }
            return 1;
        }
        return 1;
    }

    if (g_conn.state == TCP_ESTABLISHED) {
        if (flags & TCP_FLAG_RST) {
            uart_puts("tcp: RST -> CLOSED\n");
            g_conn.state = TCP_LISTEN;
            return 1;
        }
        if (data_len > 0) {
            /* Echo characters back, advance peer_seq, ACK. */
            char echo[80];
            int n = data_len < (int)sizeof(echo) ? data_len : (int)sizeof(echo);
            for (int i = 0; i < n; i++) echo[i] = (char)data[i];
            g_conn.peer_seq = seq + data_len;
            tcp_send(TCP_FLAG_PSH | TCP_FLAG_ACK, echo, n);
            g_conn.my_seq += n;

            /* If we saw a newline, close the connection. */
            for (int i = 0; i < n; i++) {
                if (echo[i] == '\r' || echo[i] == '\n') {
                    tcp_send(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
                    g_conn.my_seq += 1;
                    g_conn.state = TCP_FIN_WAIT_1;
                    uart_puts("tcp: FIN sent (got newline)\n");
                    break;
                }
            }
            return 1;
        }
        if (flags & TCP_FLAG_FIN) {
            g_conn.peer_seq = seq + 1;
            tcp_send(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
            g_conn.my_seq += 1;
            g_conn.state = TCP_LAST_ACK;
            uart_puts("tcp: peer FIN, sent FIN+ACK\n");
            return 1;
        }
        return 1;
    }

    if (g_conn.state == TCP_FIN_WAIT_1) {
        if ((flags & TCP_FLAG_ACK) && ack == g_conn.my_seq) {
            if (flags & TCP_FLAG_FIN) {
                g_conn.peer_seq = seq + 1;
                tcp_send(TCP_FLAG_ACK, 0, 0);
                g_conn.state = TCP_LISTEN;
                uart_puts("tcp: closed (back to LISTEN)\n");
            }
            return 1;
        }
        return 1;
    }

    if (g_conn.state == TCP_LAST_ACK) {
        if ((flags & TCP_FLAG_ACK) && ack == g_conn.my_seq) {
            g_conn.state = TCP_LISTEN;
            uart_puts("tcp: LAST_ACK -> LISTEN\n");
        }
        return 1;
    }

    return 1;
}

/* On-demand state dump for the shell `tcpstat` command — reads cleanly
 * even after the boot log has scrolled away.  Run `nc <ip> 23` then
 * `tcpstat`:
 *   seg_rx=0  -> the SYN never reached us (routing / dispatch / RX)
 *   seg_rx>0, syn>0, synack>0, estab=0 -> SYN+ACK left but the client's
 *              ACK never matched (bad checksum/seq, or reply dropped)
 *   txfail>0  -> the SYN+ACK could not be transmitted (TX wedge) */
void tcp_dump_state(void)
{
    const char *st;
    switch (g_conn.state) {
        case TCP_CLOSED:      st = "CLOSED";      break;
        case TCP_LISTEN:      st = "LISTEN";      break;
        case TCP_SYN_RCVD:    st = "SYN_RCVD";    break;
        case TCP_ESTABLISHED: st = "ESTABLISHED"; break;
        case TCP_FIN_WAIT_1:  st = "FIN_WAIT_1";  break;
        case TCP_LAST_ACK:    st = "LAST_ACK";    break;
        default:              st = "?";           break;
    }
    uart_puts("tcp: state="); uart_puts(st);
    uart_puts(" listen_port="); puts_u32_dec(g_listen_port);
    uart_puts("\n     any=");    puts_u32_dec(g_tcp_any);
    uart_puts(" seg_rx=");        puts_u32_dec(g_seg_rx);
    uart_puts(" syn=");          puts_u32_dec(g_syn_seen);
    uart_puts(" synack=");       puts_u32_dec(g_synack_sent);
    uart_puts(" estab=");        puts_u32_dec(g_estab);
    uart_puts(" txfail=");       puts_u32_dec(g_txfail);
    uart_puts("\n     last_peer=");
    if (g_last_port) { puts_ip(g_last_ip); uart_puts(":"); puts_u32_dec(g_last_port); }
    else             { uart_puts("(none)"); }
    uart_puts("\n");
}

/* Numeric getters for the live HDMI status window (no keyboard needed
 * to run `tcpstat`, so these drive the on-screen Network panel). */
unsigned long tcp_any_count(void)     { return g_tcp_any;     }
unsigned long tcp_seg_rx_count(void)  { return g_seg_rx;      }
unsigned long tcp_syn_count(void)     { return g_syn_seen;    }
unsigned long tcp_synack_count(void)  { return g_synack_sent; }
unsigned long tcp_estab_count(void)   { return g_estab;       }
unsigned long tcp_txfail_count(void)  { return g_txfail;      }

const char *tcp_state_str(void)
{
    switch (g_conn.state) {
        case TCP_CLOSED:      return "CLOSED";
        case TCP_LISTEN:      return "LISTEN";
        case TCP_SYN_RCVD:    return "SYN_RCVD";
        case TCP_ESTABLISHED: return "ESTAB";
        case TCP_FIN_WAIT_1:  return "FINWAIT1";
        case TCP_LAST_ACK:    return "LASTACK";
        default:              return "?";
    }
}
