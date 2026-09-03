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
#include "actor.h"
#include "vfs.h"
#include "wm.h"
#include "wifi.h"

extern int genet_tx_frame(const unsigned char *frame, int length);

/* TX is pluggable so the same server runs over Ethernet (genet, Pi 4) or the
 * WiFi data path (wifi_eth_tx, Pi 5).  Defaults to genet; the WiFi bring-up
 * calls tcp_set_tx(wifi_eth_tx) once it has a DHCP address. */
static int (*g_tcp_tx)(const unsigned char *frame, int length) = genet_tx_frame;
void tcp_set_tx(int (*fn)(const unsigned char *frame, int length)) { g_tcp_tx = fn; }

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

/* --- Send path: segmentation + retransmission ---------------------------
 *
 * tcp_send() refuses anything over one frame while the HTTP layer handed it
 * the whole response in one call, so any body over ~1.4 KB was dropped
 * outright — the FIN still went out, so the client saw a clean close with
 * zero bytes.  (`/fb` is hand-chunked to dodge this; nothing else was, and
 * http_resp was capped at 1400 to hide it.)
 *
 * Now the connection owns a send buffer that holds the response until the
 * peer acknowledges it.  tcp_pump() emits MSS-sized segments up to the
 * in-flight limit, ACKs slide snd_una forward, and the RX tick retransmits
 * from snd_una when the RTO expires. */
#define TCP_MSS          1460           /* 1500 MTU - 20 IP - 20 TCP */
#define TCP_SND_BUF      16384
#define TCP_MAX_INFLIGHT (4 * TCP_MSS)  /* our own cap, on top of peer's win */
#define TCP_RTO_MS       500UL
#define TCP_MAX_RETRIES  6

struct tcp_conn {
    int state;
    unsigned char peer_mac[6];
    unsigned char peer_ip[4];
    unsigned short peer_port;
    unsigned long my_seq;       /* next byte we will send */
    unsigned long peer_seq;     /* next byte we expect to receive */
    int greeted;
    /* Wall-clock stamp of the last segment seen on this conn.  Without it
     * the connection only ever advances on receive, so a single bare SYN
     * whose final ACK never arrives pins the one slot in SYN_RCVD forever
     * and the server is dead until reboot.  tcp_conn_reap() uses this. */
    unsigned long last_ms;

    /* Unacknowledged outbound data.  snd_seq0 is the sequence number of
     * snd[0]; snd_una is how much the peer has acknowledged and snd_nxt how
     * much we have put on the wire, both as offsets into snd[]. */
    char           snd[TCP_SND_BUF];
    int            snd_len;
    int            snd_una;
    int            snd_nxt;
    unsigned long  snd_seq0;
    unsigned long  snd_last_tx_ms;
    int            snd_retries;
    int            snd_fin_pending;   /* close once the last data is on the wire */
    int            peer_fin;          /* peer half-closed (CLOSE_WAIT) */
    int            fin_acked;         /* peer acknowledged OUR fin */
    unsigned short peer_win;          /* peer's advertised receive window */
};

static struct tcp_conn g_conn;
static int g_reqlen;                  /* bytes of HTTP request accumulated so far */
static int g_chain_len;               /* bytes of a kernel image staged for /chainload */
#define CHAIN_STAGE_CAP  (4UL * 1024 * 1024)
static unsigned char *g_chain_stage;  /* heap staging buffer, above _end */
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

    return g_tcp_tx((const unsigned char *)tx_frame, send_len);
}

/* =====================================================================
 * Minimal HTTP/1.0 layer (Phase H1/B1).
 *
 * On an ESTABLISHED connection the first data segment is treated as an
 * HTTP request.  We route the GET path to the actor layer and reply
 * with a small JSON (or HTML) body, then close.  No libc, so tiny
 * string helpers are inlined.  Routes:
 *   GET /                         -> text status
 *   GET /api/actors               -> JSON actor list
 *   GET /send?to=N&m=METHOD&arg=X -> deliver msg to actor N, JSON result
 * =================================================================== */

static int s_put(char *b, int pos, const char *s)
{
    while (*s) b[pos++] = *s++;
    return pos;
}

static int s_putdec(char *b, int pos, long v)
{
    char t[16]; int n = 0;
    if (v < 0) { b[pos++] = '-'; v = -v; }
    if (v == 0) { b[pos++] = '0'; return pos; }
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) b[pos++] = t[n];
    return pos;
}

static int s_puthex(char *b, int pos, unsigned int v)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) b[pos++] = hx[(v >> (i*4)) & 0xF];
    return pos;
}

/* ---------- /fs/* hierarchical-filesystem HTTP helpers ---------- */

/* Copy the request-line path (after "METHOD ", up to ' ' or '?') into pbuf. */
static int http_path(const char *req, char *pbuf, int max)
{
    int s = 0;
    while (req[s] && req[s] != ' ') s++;          /* skip the method */
    if (req[s] == ' ') s++;
    int i = 0;
    while (req[s] && req[s] != ' ' && req[s] != '?' && i < max - 1) pbuf[i++] = req[s++];
    pbuf[i] = 0;
    return i;
}

static int str_starts(const char *s, const char *pfx)
{
    while (*pfx) { if (*s != *pfx) return 0; s++; pfx++; }
    return 1;
}

/* HTTP body (after the blank CRLFCRLF line), or NULL. */
static const char *http_body(const char *req)
{
    int n = 0; while (req[n]) n++;
    for (int i = 0; i + 3 < n; i++)
        if (req[i]=='\r'&&req[i+1]=='\n'&&req[i+2]=='\r'&&req[i+3]=='\n') return req + i + 4;
    return 0;
}

/* Is the accumulated request complete?  Needs the CRLFCRLF header terminator;
 * if a Content-Length header is present, also needs that many body bytes.
 * Lets the server reassemble a POST body that spans several TCP segments. */
static int http_complete(const char *buf, int len)
{
    int he = -1;
    for (int i = 0; i + 3 < len; i++)
        if (buf[i]=='\r'&&buf[i+1]=='\n'&&buf[i+2]=='\r'&&buf[i+3]=='\n') { he = i + 4; break; }
    if (he < 0) return 0;                       /* headers not finished yet */
    long cl = -1;
    for (int i = 0; i + 15 < he; i++) {
        const char *p = "content-length:";
        int k = 0;
        while (p[k]) {
            char c = buf[i + k];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != p[k]) break;
            k++;
        }
        if (!p[k]) {                            /* matched the header name */
            int j = i + k;
            while (buf[j] == ' ') j++;
            cl = 0;
            while (buf[j] >= '0' && buf[j] <= '9') cl = cl * 10 + (buf[j++] - '0');
            break;
        }
    }
    if (cl < 0) return 1;                        /* no body expected (GET) */
    return (len >= he + (int)cl) ? 1 : 0;
}

/* Parse the Content-Length header value (or -1). */
static int content_length(const char *req)
{
    int n = 0; while (req[n]) n++;
    for (int i = 0; i + 15 < n; i++) {
        const char *p = "content-length:"; int k = 0;
        while (p[k]) { char c = req[i+k]; if (c>='A'&&c<='Z') c=(char)(c+32); if (c!=p[k]) break; k++; }
        if (!p[k]) { int j=i+k; while (req[j]==' ') j++; int v=0; while (req[j]>='0'&&req[j]<='9') v=v*10+(req[j++]-'0'); return v; }
    }
    return -1;
}

/* vfs_walk callback: render an indented tree into a capped buffer. */
struct treectx { char *b; int bl; int max; };
static void tree_visit(int depth, vfs_node_t *n, void *c)
{
    struct treectx *t = (struct treectx *)c;
    if (t->bl >= t->max - 48) return;
    for (int d = 0; d < depth && t->bl < t->max - 4; d++) t->bl = s_put(t->b, t->bl, "  ");
    t->bl = s_put(t->b, t->bl, n->name[0] ? n->name : "/");
    if (n->kind == VFS_DIR) t->b[t->bl++] = '/';
    else { t->bl = s_put(t->b, t->bl, "  ("); t->bl = s_putdec(t->b, t->bl, (long)n->size); t->bl = s_put(t->b, t->bl, "B)"); }
    t->b[t->bl++] = '\n';
}

/* Locate "key=" in NUL-terminated `req` and copy its value (up to the
 * next '&', ' ' or end) into out[0..max-1].  Returns 1 if found. */
static int q_param(const char *req, const char *key, char *out, int max)
{
    for (const char *p = req; *p; p++) {
        const char *k = key; const char *q = p;
        while (*k && *q == *k) { q++; k++; }
        if (*k == 0 && *q == '=') {       /* matched "key=" */
            q++;
            int i = 0;
            while (*q && *q != '&' && *q != ' ' && i < max - 1)
                out[i++] = *q++;
            out[i] = 0;
            return 1;
        }
    }
    out[0] = 0;
    return 0;
}

static int q_int(const char *req, const char *key, int dflt)
{
    char v[16];
    if (!q_param(req, key, v, sizeof v)) return dflt;
    int neg = 0, i = 0, n = 0;
    if (v[0] == '-') { neg = 1; i = 1; }
    for (; v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + (v[i] - '0');
    return neg ? -n : n;
}

/* Path begins right after "GET " and runs to the next space. */
static int path_eq(const char *req, const char *lit)
{
    if (req[0]!='G'||req[1]!='E'||req[2]!='T'||req[3]!=' ') return 0;
    const char *p = req + 4;
    while (*lit && *p == *lit) { p++; lit++; }
    return *lit == 0 && (*p == ' ' || *p == '?');
}

/* ---- SMP worker pool (system/smp.c) ---- */
extern int  smp_cores_online(void);
typedef long (*smp_range_fn)(long lo, long hi, int core);
extern long smp_parallel_sum(smp_range_fn fn, long n, int ncores);

/* Free-running counter in milliseconds (CNTPCT_EL0 / CNTFRQ_EL0 * 1000). */
static unsigned long now_ms(void)
{
    unsigned long ct, hz;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(ct));
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(hz));
    if (!hz) return 0;
    return (ct * 1000UL) / hz;
}

/* ---- Send pump: emit as much of snd[] as the window allows -------------
 *
 * Called after queueing a response, when an ACK opens the window, and from
 * the retransmit path.  Sends whole MSS segments (the tail may be short),
 * bounded by both the peer's advertised window and our own in-flight cap so
 * a large response can't outrun the single-frame TX path.
 *
 * The FIN goes out in the same flight as the last data segment, as a normal
 * stack does — it just occupies the next sequence number and the retransmit
 * timer covers it.  Holding it until every byte was ACKed costs a full round
 * trip per response; on the Pi 4 that measured ~1.8x slower overall. */
static int  tcp_send(unsigned char flags, const char *payload, int payload_len);
static unsigned long now_ms(void);
static unsigned long g_retrans;         /* segments retransmitted */
/* Post-mortem of the PREVIOUS connection.  The send state lives in g_conn and
 * is reset by the next SYN, so a stalled transfer cannot be inspected from a
 * fresh HTTP request — by the time /tcpstat is served, the evidence is gone.
 * These are snapshotted just before the reset. */
static long g_snap_len, g_snap_nxt, g_snap_una, g_snap_win, g_snap_state;
static unsigned long g_pump_calls, g_segs_tx, g_ack_calls, g_tick_calls;

static void tcp_pump(void)
{
    struct tcp_conn *c = &g_conn;
    g_pump_calls++;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_FIN_WAIT_1) return;

    int limit = TCP_MAX_INFLIGHT;
    if (c->peer_win && c->peer_win < limit) limit = c->peer_win;
    if (limit < TCP_MSS) limit = TCP_MSS;      /* always make progress */

    while (c->snd_nxt < c->snd_len) {
        int inflight = c->snd_nxt - c->snd_una;
        if (inflight >= limit) break;

        int n = c->snd_len - c->snd_nxt;
        if (n > TCP_MSS) n = TCP_MSS;

        c->my_seq = c->snd_seq0 + (unsigned long)c->snd_nxt;
        if (tcp_send(TCP_FLAG_PSH | TCP_FLAG_ACK, c->snd + c->snd_nxt, n) < 0) {
            g_txfail++;
            break;                              /* TX busy — retry on the tick */
        }
        c->snd_nxt += n;
        g_segs_tx++;
        c->snd_last_tx_ms = now_ms();
    }

    /* Leave my_seq pointing at the next NEW byte, so a pure ACK emitted from
     * some other path doesn't carry a mid-stream sequence number. */
    if (c->snd_len > 0) c->my_seq = c->snd_seq0 + (unsigned long)c->snd_nxt;

    if (c->snd_fin_pending && c->snd_nxt >= c->snd_len) {
        c->my_seq = c->snd_seq0 + (unsigned long)c->snd_len;
        if (tcp_send(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0) >= 0) {
            c->my_seq += 1;
            c->snd_fin_pending = 0;
            /* If the peer already half-closed we owe only the final ACK. */
            c->state = c->peer_fin ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
            c->snd_last_tx_ms = now_ms();
        }
    }
}

/* Queue a response body and start sending it. */
static void tcp_send_response(const char *body, int len)
{
    struct tcp_conn *c = &g_conn;
    if (len < 0) len = 0;
    if (len > TCP_SND_BUF) len = TCP_SND_BUF;   /* truncate, but honestly */
    for (int i = 0; i < len; i++) c->snd[i] = body[i];
    c->snd_len         = len;
    c->snd_una         = 0;
    c->snd_nxt         = 0;
    c->snd_seq0        = c->my_seq;
    c->snd_retries     = 0;
    c->snd_fin_pending = 1;
    c->snd_last_tx_ms  = now_ms();
    tcp_pump();
}

/* Peer acknowledged up to `ack`: slide the window and send more. */
static void tcp_on_ack(unsigned long ack)
{
    struct tcp_conn *c = &g_conn;
    g_ack_calls++;
    if (c->snd_len > 0) {
        unsigned long acked = ack - c->snd_seq0;      /* wraps correctly */
        if (acked <= (unsigned long)c->snd_len && (int)acked > c->snd_una) {
            c->snd_una     = (int)acked;
            c->snd_retries = 0;
        }
    }
    tcp_pump();
}

/* Retransmit timer.  Driven from the RX path alongside the reaper, so it
 * needs no timer of its own; the cost is that it only fires while packets
 * are flowing or the loop is idling, which is exactly when it matters. */
void tcp_retransmit_tick(void)
{
    struct tcp_conn *c = &g_conn;
    g_tick_calls++;
    if (c->snd_una >= c->snd_len && !c->snd_fin_pending) return;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_FIN_WAIT_1) return;

    unsigned long now = now_ms();
    if (c->snd_last_tx_ms == 0 || now < c->snd_last_tx_ms) {
        c->snd_last_tx_ms = now;
        return;
    }
    if (now - c->snd_last_tx_ms < TCP_RTO_MS) return;

    if (++c->snd_retries > TCP_MAX_RETRIES) {
        /* Peer is gone.  Drop the response rather than retransmitting for
         * ever; the reaper will recycle the slot. */
        c->snd_len = c->snd_una = c->snd_nxt = 0;
        c->snd_fin_pending = 0;
        return;
    }
    c->snd_nxt = c->snd_una;          /* go-back-N */
    g_retrans++;
    tcp_pump();
}

/* ---- half-open / idle connection reaper -------------------------------
 * There is one connection slot and no retransmit timer, so a peer that
 * sends a bare SYN and never completes the handshake owns the server
 * until reboot.  Recycle the slot on a timeout instead.  Called from the
 * RX path (loader/main.c, device/wifi/wifi.c) so it runs without needing
 * its own timer. */
#define TCP_HALFOPEN_TIMEOUT_MS  5000UL
#define TCP_ESTAB_TIMEOUT_MS    60000UL

static unsigned long g_reaped;          /* slots recycled by the reaper */

void tcp_conn_reap(void)
{
    struct tcp_conn *c = &g_conn;
    if (c->state == TCP_CLOSED || c->state == TCP_LISTEN) return;

    unsigned long now = now_ms();
    unsigned long limit = (c->state == TCP_ESTABLISHED)
                        ? TCP_ESTAB_TIMEOUT_MS
                        : TCP_HALFOPEN_TIMEOUT_MS;
    /* now_ms() is monotonic here (free-running 64-bit counter), so a plain
     * subtraction is safe; guard against a zero stamp anyway. */
    if (c->last_ms == 0 || now < c->last_ms) { c->last_ms = now; return; }
    if (now - c->last_ms < limit) return;

    c->state   = TCP_LISTEN;
    c->greeted = 0;
    g_reqlen   = 0;
    g_reaped++;
}

/* Microsecond timer off the same generic counter — the ms version rounds too
 * coarsely for the sub-millisecond fill benchmark.  Matches the resolution of
 * the Pi 3's clkcount() so all three boards' numbers are comparable. */
static unsigned long now_us(void)
{
    unsigned long ct, hz;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(ct));
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(hz));
    if (!hz) return 0;
    return (ct * 1000000UL) / hz;
}

/* SMP benchmark kernel: count primes in [lo,hi) by trial division.  Pure
 * integer compute (mul/mod in registers) so it scales with CPU cores, not
 * memory bandwidth (the D-cache is off — see mmu.c).  Matches smp_range_fn. */
static long bench_count_primes(long lo, long hi, int core)
{
    (void)core;
    if (lo < 2) lo = 2;
    long cnt = 0;
    for (long n = lo; n < hi; n++) {
        int prime = 1;
        for (long d = 2; d * d <= n; d++) {
            if (n % d == 0) { prime = 0; break; }
        }
        cnt += prime;
    }
    return cnt;
}

/* ---- N-Queens (SMP): count solutions with the first queen pinned to each
 * column in [lo,hi).  Summing over all n columns gives the full count, so the
 * 1-core and N-core runs MUST agree (correctness check).  g_nq_n holds the
 * board size, set by the /bench route before dispatch.  Bitmask backtracking,
 * pure integer compute → scales with cores (D-cache is off). */
static int g_nq_n = 13;
static long nq_solve(int row, unsigned cols, unsigned d1, unsigned d2, unsigned all)
{
    (void)row;
    if (cols == all) return 1;
    long count = 0;
    unsigned avail = ~(cols | d1 | d2) & all;
    while (avail) {
        unsigned bit = avail & (unsigned)(-(long)avail);   /* lowest set bit */
        avail -= bit;
        count += nq_solve(row + 1, cols | bit, (d1 | bit) << 1, (d2 | bit) >> 1, all);
    }
    return count;
}
static long bench_nqueens(long lo, long hi, int core)
{
    (void)core;
    int n = g_nq_n;
    unsigned all = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    long total = 0;
    for (long c = lo; c < hi && c < n; c++) {
        unsigned bit = 1u << c;
        total += nq_solve(1, bit, bit << 1, bit >> 1, all);
    }
    return total;
}

/* ---- Dining Philosophers (SMP): run independent dining tables in [lo,hi).
 * Each table = g_din_n philosophers doing DIN_ROUNDS deadlock-free meal cycles
 * (Dijkstra resource hierarchy: acquire the lower-numbered fork first), with a
 * little per-meal compute so wall-time is meaningful.  Tables are independent,
 * so splitting them across cores is an honest throughput speedup.  1-core and
 * N-core run the same total table count → meal totals MUST agree.  Returns the
 * total number of meals eaten across [lo,hi). */
#define DIN_ROUNDS 24
static int g_din_n = 5;
static volatile unsigned g_din_sink = 0;
static long bench_dining(long lo, long hi, int core)
{
    (void)core;
    int np = g_din_n;
    long meals = 0;
    unsigned acc = (unsigned)lo * 2654435761u;
    for (long t = lo; t < hi; t++) {
        for (int round = 0; round < DIN_ROUNDS; round++) {
            for (int p = 0; p < np; p++) {
                int l = p, r = (p + 1) % np;
                int fa = (l < r) ? l : r;          /* lower-numbered fork first */
                int fb = (l < r) ? r : l;
                acc = acc * 1103515245u + 12345u + (unsigned)(fa * 131 + fb);
                meals++;
            }
        }
    }
    g_din_sink ^= acc;
    return meals;
}

/* ---- Memory-fill benchmark (cross-board: does bulk store scale on A76?) ----
 * Identical workload to the Pi 3/Pi 4 fill: a plain buffer written 8 times.
 * With the D-cache off (as on all three boards) every store is a RAM
 * transaction, so this is memory-bound, exactly like framebuffer drawing.
 * smp_parallel_sum splits [0,n) into contiguous per-core bands. */
#define BENCH_FILL_WORDS  (256 * 1024)   /* 1 MB, matches Pi 3/Pi 4 */
#define BENCH_FILL_PASSES 8
static unsigned bench_fill_buf[BENCH_FILL_WORDS];
static long bench_fill(long lo, long hi, int core)
{
    volatile unsigned *b = bench_fill_buf;
    (void)core;
    for (long p = 0; p < BENCH_FILL_PASSES; p++)
        for (long i = lo; i < hi; i++) b[i] = (unsigned)(i + p);
    return hi - lo;
}

/* Pool round-trip: does nothing, so the N-core time is dispatch+collect
 * overhead — the parallelise/don't break-even point. */
static long bench_null(long lo, long hi, int core)
{
    (void)core;
    return hi - lo;
}

/* ---- D-cache experiment: run the sweep to the SERIAL console --------------
 * Used only by the DCACHE_EXPERIMENT build.  With the D-cache ON there is no
 * network (the GEM DMA rings are coherent only while the cache is off), so the
 * result cannot be served over HTTP; this prints it over UART instead.  Called
 * from main() right after smp_init(), BEFORE any DMA agent (net/USB/WM) starts,
 * so the D-cache never has to coexist with an incoherent device — the only
 * memory touched is the private bench_fill_buf and pure compute.  Same
 * workloads and timing (now_us) as the HTTP /bench, so the numbers compare
 * directly with the Pi 3 D-cache experiment. */
void smpbench_serial_run(void)
{
    static char b[128];
    static const long fills[] = { 4096, 16384, 65536, 262144 };
    int online = smp_cores_online();
    unsigned long sctlr;
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));

    uart_puts("\r\n===== smpbench (D-cache experiment) =====\r\n");
    uart_puts("cores_online = "); uart_putc((char)('0' + online)); uart_puts("\r\n");
    { int n = s_put(b, 0, "sctlr = 0x"); n = s_puthex(b, n, (unsigned int)sctlr);
      n = s_put(b, n, "  (C=");
      n = s_put(b, n, (sctlr & (1UL<<2)) ? "1 ON" : "0 OFF"); n = s_put(b, n, ")\r\n");
      b[n]=0; uart_puts(b); }
    uart_puts("compare speedup RATIOS (clock varies); agree=yes = coherent.\r\n");

    /* dining (pure compute) + fill sweep (memory) across 1..online cores. */
    for (int pass = 0; pass < 3 + (int)(sizeof(fills)/sizeof(fills[0])); pass++) {
        smp_range_fn fn; long units; const char *label;
        if (pass == 0)      { g_din_n = 5;  units = 200000;  fn = bench_dining;  label = "dining"; }
        else if (pass == 1) { g_nq_n = 12;  units = g_nq_n;  fn = bench_nqueens; label = "nqueens"; }
        else if (pass == 2) { units = online; fn = bench_null; label = "null"; }
        else { units = fills[pass-3]; fn = bench_fill; label = "fill"; }

        unsigned long t0 = now_us();
        long r1 = smp_parallel_sum(fn, units, 1);
        unsigned long us1 = now_us() - t0;
        t0 = now_us();
        long rN = smp_parallel_sum(fn, units, online);
        unsigned long usN = now_us() - t0;

        int n = s_put(b, 0, "  "); n = s_put(b, n, label);
        if (fn == bench_fill || fn == bench_nqueens) { n = s_put(b, n, " n="); n = s_putdec(b, n, units); }
        n = s_put(b, n, "  1c="); n = s_putdec(b, n, (long)us1);
        n = s_put(b, n, "us Nc="); n = s_putdec(b, n, (long)usN);
        n = s_put(b, n, "us  x100=");
        n = s_putdec(b, n, usN ? (long)((us1 * 100UL) / usN) : 0);
        n = s_put(b, n, "  agree="); n = s_put(b, n, (r1 == rN) ? "yes" : "NO");
        n = s_put(b, n, "\r\n"); b[n]=0; uart_puts(b);
    }
    uart_puts("===== end smpbench =====\r\n");
}

/* ---- N-Queens partial (distributed): count solutions for first-queen columns
 * [g_nqp_c0+lo, g_nqp_c0+hi) at board size g_nqp_n.  Used by /nqpart so a Mac
 * orchestrator can hand each board a disjoint column range and sum the partial
 * counts.  Native (no JIT deadline) → correct for any n; SMP across cores. */
static int g_nqp_n = 13;
static int g_nqp_c0 = 0;
static long bench_nqueens_part(long lo, long hi, int core)
{
    (void)core;
    int n = g_nqp_n;
    unsigned all = (n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    long total = 0;
    for (long c = lo; c < hi; c++) {
        unsigned col = (unsigned)(g_nqp_c0 + c);
        unsigned bit = 1u << col;
        total += nq_solve(1, bit, bit << 1, bit >> 1, all);
    }
    return total;
}

/* Build the HTTP response for NUL-terminated request `req` into `out`
 * (capacity `max`).  Returns the byte length. */
/* ---- /wifi-adhoc : join the WiFi ad-hoc (IBSS) mesh, keep Ethernet ----
 * The Mesh Control Center hits GET /wifi-adhoc?ssid=NAME&ch=N&n=M to put this
 * board on the 10.0.0.M MANET cell.  WiFi bring-up blocks the (single) main
 * loop for ~1 min, so we DON'T run it inside the request — we stash the params
 * and let the main-loop poller (wifi_adhoc_poll_pending, called from
 * serial_io_tick) run it once, AFTER this response is flushed.  keep_eth=1
 * keeps the HTTP gateway + /fb mirror on Ethernet while WiFi joins the mesh. */
static volatile int  g_adhoc_pending = 0;
static char          g_adhoc_ssid[40];
static int           g_adhoc_ch = 6, g_adhoc_n = 0;

void wifi_adhoc_poll_pending(void)
{
    if (!g_adhoc_pending) return;
    g_adhoc_pending = 0;
    extern int wifi_adhoc_keep_eth;
    extern int wifi_adhoc(const char *ssid, int channel, int n);
    wifi_adhoc_keep_eth = 1;                 /* stay on Ethernet (mesh+mirror) */
    wifi_adhoc(g_adhoc_ssid, g_adhoc_ch, g_adhoc_n);
}

/* ===== RT diagnostic harness (rpi5): periodic-task release jitter =========
 * Ported from xinu-rpi4's /rtos-jitter, adapted to rpi5's proc API:
 *   - proc_create_static (NOT proc_create): heap-free, safe from the
 *     genet_rx_tick/net_proc context this runs in (getmem is non-reentrant here);
 *   - proc_create_static already ready_push()es, so we do NOT call proc_ready();
 *   - newly-created procs start IRQs-on (proc_entry_trampoline), so a compute
 *     hog is genuinely preemptible.
 * Since RT-port Stage 2, http_build runs in the net_proc process, so proc_yield/
 * proc_sleep_us here are safe.  rpi5 has priority ready_pop + tickless
 * proc_sleep_us + timer preemption, so a high-prio periodic task should hold its
 * period even under a full-core hog. */
extern int  proc_create_static(void (*)(void), void *, unsigned long, const char *);
extern void proc_yield(void);
extern void proc_exit(void);
extern void proc_setprio(int, int);
extern void proc_sleep_us(unsigned long);
extern void proc_set_preempt(int);

static unsigned char g_rt_stk_a[8192];   /* hog / preempt-A (reused per request) */
static unsigned char g_rt_stk_b[8192];   /* measure / preempt-B                  */

struct jit_stat {
    volatile int  done;
    unsigned long target_us, n, min_us, max_us, sum_us, max_abs_jit_us, misses;
};
static struct jit_stat        g_jit;
static volatile int           g_jit_hog_run;
static volatile unsigned long g_jit_chunk_us;

static void jit_hog(void)
{
    /* IRQs on at entry (trampoline) so the timer preempts this tight loop. */
    volatile unsigned x = 2463534242u;
    while (g_jit_hog_run) {
        unsigned long s = now_us();
        while (now_us() - s < g_jit_chunk_us) {
            int i; for (i = 0; i < 64; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
        }
        proc_yield();                    /* cooperative fallback if preempt off */
    }
    proc_exit();
}

static void jit_measure(void)
{
    /* Relative schedule: each release measured `target` after the ACTUAL prev
     * wake, so a late wake never triggers a catch-up burst.  Publish once. */
    unsigned long target = g_jit.target_us, N = g_jit.n;
    unsigned long mn = 0xffffffffUL, mx = 0, sum = 0, mj = 0, miss = 0;
    unsigned long prev = now_us(), i;
    for (i = 0; i < N; i++) {
        proc_sleep_us(target);                       /* tickless RT sleep */
        unsigned long t = now_us(), d = t - prev;    /* actual period */
        prev = t;
        if (d < mn) mn = d;
        if (d > mx) mx = d;
        sum += d;
        unsigned long jit = (d > target) ? (d - target) : (target - d);
        if (jit > mj) mj = jit;
        if (jit > target / 2UL) miss++;              /* deadline miss = >50% off */
    }
    g_jit.min_us = mn; g_jit.max_us = mx; g_jit.sum_us = sum;
    g_jit.max_abs_jit_us = mj; g_jit.misses = miss; g_jit.n = N;
    g_jit_hog_run = 0;
    g_jit.done = 1;
    proc_exit();
}

static void jit_run(unsigned long period_ms, unsigned long samples,
                    int with_hog, unsigned long chunk_us)
{
    g_jit.done = 0; g_jit.target_us = period_ms * 1000UL; g_jit.n = samples;
    if (with_hog) {
        g_jit_hog_run = 1; g_jit_chunk_us = chunk_us;
        int hp = proc_create_static(jit_hog, g_rt_stk_a, sizeof g_rt_stk_a, "jit_hog");
        if (hp > 0) proc_setprio(hp, 5);             /* low priority; already ready */
    }
    int mp = proc_create_static(jit_measure, g_rt_stk_b, sizeof g_rt_stk_b, "jit_meas");
    if (mp <= 0) { g_jit.done = 1; g_jit_hog_run = 0; return; }
    proc_setprio(mp, 50);                            /* high (RT) priority */
    /* Time-based guard: run lasts ~samples*(period+chunk); yield until done. */
    unsigned long deadline = now_us()
        + (unsigned long)samples * (period_ms * 1000UL + chunk_us) + 3000000UL;
    while (!g_jit.done && now_us() < deadline) proc_yield();
    g_jit_hog_run = 0;
    { int k; for (k = 0; k < 256; k++) proc_yield(); }   /* let the hog exit */
}

/* ---- /preempt demo: two equal-priority CPU tasks, no yield ----
 * Under preemptive scheduling the 100 Hz tick time-slices them -> interleaved
 * "ABAB..."; cooperative (preempt off) -> blocky "AAAA..BBBB". */
static volatile char g_pd_log[40];
static volatile int  g_pd_pos;
static void pd_mark(char c)
{
    int reps;
    for (reps = 0; reps < 6; reps++) {
        unsigned long s = now_us();
        while (now_us() - s < 4000UL) { }            /* ~4 ms busy, NO yield */
        int q = g_pd_pos;
        if (q < (int)sizeof(g_pd_log) - 1) { g_pd_log[q] = c; g_pd_pos = q + 1; }
    }
}
static void pd_a(void) { pd_mark('A'); proc_exit(); }
static void pd_b(void) { pd_mark('B'); proc_exit(); }

static int http_build(const char *req, char *out, int max)
{
    char body[640];
    int  bl = 0;
    const char *ctype = "application/json";
    (void)max;

    char rpath[300];
    http_path(req, rpath, sizeof rpath);          /* request path, for /fs/* */

    if (path_eq(req, "/wifi-adhoc")) {
        char sb[40];
        if (!q_param(req, "ssid", sb, sizeof sb) || !sb[0]) {
            sb[0]='M'; sb[1]='A'; sb[2]='N'; sb[3]='E'; sb[4]='T'; sb[5]=0;
        }
        int ch = q_int(req, "ch", 6);
        int nn = q_int(req, "n", 0);
        int i = 0; for (; sb[i] && i < (int)sizeof(g_adhoc_ssid)-1; i++) g_adhoc_ssid[i] = sb[i];
        g_adhoc_ssid[i] = 0;
        g_adhoc_ch = ch; g_adhoc_n = nn;
        g_adhoc_pending = 1;                 /* main loop runs the join shortly */

        int p = 0;
        p = s_put(out, p, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
        p = s_put(out, p, "adhoc queued ssid=");
        p = s_put(out, p, g_adhoc_ssid);
        p = s_put(out, p, " ch=");      p = s_putdec(out, p, ch);
        p = s_put(out, p, " ip=10.0.0."); p = s_putdec(out, p, nn);
        p = s_put(out, p, " (gateway stays on ethernet; wifi joins mesh in ~1 min)\n");
        return p;
    }

    if (path_eq(req, "/avm-par")) {
        /* Toggle/inspect the multi-core actor scheduler at runtime (no reflash).
         *   GET /avm-par           -> report state + diag counters
         *   GET /avm-par?on=1|0    -> enable/disable parallel dispatch */
        extern void avm_set_par(int on);
        extern int  avm_get_par(void), avm_get_par_nbatch(void), avm_get_par_lastbn(void);
        extern int  smp_cores_online(void);
        int on = q_int(req, "on", -1);       /* absent -> -1 -> leave unchanged */
        if (on >= 0) avm_set_par(on);
        int p = 0;
        p = s_put(out, p, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
        p = s_put(out, p, "avm-par enable=");  p = s_putdec(out, p, avm_get_par());
        p = s_put(out, p, " cores_online="); p = s_putdec(out, p, smp_cores_online());
        p = s_put(out, p, " nbatch=");       p = s_putdec(out, p, avm_get_par_nbatch());
        p = s_put(out, p, " lastbn=");       p = s_putdec(out, p, avm_get_par_lastbn());
        p = s_put(out, p, "\n");
        return p;
    }

    if (path_eq(req, "/rtos-jitter")) {
        /* Periodic-task release jitter, idle vs under a full-core compute hog.
         * rpi5 RT (preempt + tickless): a high-prio 1 kHz task should hold its
         * period with 0 deadline misses even under load.  Compare preempt on/off:
         *   curl '.../rtos-jitter?period_ms=1&samples=200&chunk_us=5000&preempt=1'
         *   curl '.../rtos-jitter?period_ms=1&samples=200&chunk_us=5000&preempt=0' */
        int P  = q_int(req, "period_ms", 1);   if (P < 1) P = 1;   if (P > 1000) P = 1000;
        int NS = q_int(req, "samples", 200);   if (NS < 1) NS = 1; if (NS > 2000) NS = 2000;
        int C  = q_int(req, "chunk_us", 5000); if (C < 0) C = 0;   if (C > 100000) C = 100000;
        int pe = q_int(req, "preempt", 1);
        proc_set_preempt(pe ? 1 : 0);
        jit_run(P, NS, 0, 0);                struct jit_stat a = g_jit;   /* idle   */
        jit_run(P, NS, 1, (unsigned long)C); struct jit_stat b = g_jit;   /* + hog  */
        proc_set_preempt(1);                 /* restore net_proc steady state */
        int p = 0;
        p = s_put(out, p, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
        p = s_put(out, p, "rtos-jitter (rpi5 RT-port Stage2: preempt+tickless) period_ms=");
        p = s_putdec(out, p, P);
        p = s_put(out, p, " samples=");      p = s_putdec(out, p, NS);
        p = s_put(out, p, " hog_chunk_us="); p = s_putdec(out, p, C);
        p = s_put(out, p, " preempt=");      p = s_putdec(out, p, pe);
        p = s_put(out, p, " target_us=");    p = s_putdec(out, p, (long)a.target_us);
        p = s_put(out, p, "\n[idle]   mean_us="); p = s_putdec(out, p, (long)(a.n ? a.sum_us / a.n : 0));
        p = s_put(out, p, " max_us=");     p = s_putdec(out, p, (long)a.max_us);
        p = s_put(out, p, " max_jit_us="); p = s_putdec(out, p, (long)a.max_abs_jit_us);
        p = s_put(out, p, " misses=");     p = s_putdec(out, p, (long)a.misses);
        p = s_put(out, p, "\n[loaded] mean_us="); p = s_putdec(out, p, (long)(b.n ? b.sum_us / b.n : 0));
        p = s_put(out, p, " max_us=");     p = s_putdec(out, p, (long)b.max_us);
        p = s_put(out, p, " max_jit_us="); p = s_putdec(out, p, (long)b.max_abs_jit_us);
        p = s_put(out, p, " misses=");     p = s_putdec(out, p, (long)b.misses);
        p = s_put(out, p, "\n");
        return p;
    }

    if (path_eq(req, "/preempt")) {
        /* Two equal-priority CPU tasks (no yield).  preempt=1 -> interleaved
         * ABAB (100 Hz time-slice); preempt=0 -> blocky AAAA..BBBB. */
        int pe = q_int(req, "preempt", 1);
        int z;
        g_pd_pos = 0;
        for (z = 0; z < (int)sizeof(g_pd_log); z++) g_pd_log[z] = 0;
        proc_set_preempt(pe ? 1 : 0);
        int ai = proc_create_static(pd_a, g_rt_stk_a, sizeof g_rt_stk_a, "pd_a");
        int bi = proc_create_static(pd_b, g_rt_stk_b, sizeof g_rt_stk_b, "pd_b");
        if (ai > 0) proc_setprio(ai, 10);
        if (bi > 0) proc_setprio(bi, 10);            /* equal priority */
        unsigned long dl = now_us() + 3000000UL;
        while (g_pd_pos < 12 && now_us() < dl) proc_yield();
        proc_set_preempt(1);                         /* restore steady state */
        char tmp[41];
        for (z = 0; z < (int)sizeof(g_pd_log) && g_pd_log[z]; z++) tmp[z] = g_pd_log[z];
        tmp[z] = 0;
        int p = 0;
        p = s_put(out, p, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
        p = s_put(out, p, "preempt="); p = s_putdec(out, p, pe);
        p = s_put(out, p, " (equal-prio A/B, ~4ms/mark, no yield): ");
        p = s_put(out, p, tmp);
        p = s_put(out, p, "\n  interleaved ABAB = preemptive; blocky AAAA..BBBB = cooperative\n");
        return p;
    }

    /* ---- /fb : remote screen mirror ------------------------------------
     * This minimal TCP stack sends a response in ONE segment (<=~1464 B), so
     * the 1280x720 framebuffer can't go in one reply.  Instead serve a small
     * RGB565 thumbnail in chunks the browser stitches back together:
     *   GET /fb[?w=W]          -> "DW DH SRCW SRCH"  (text dims)
     *   GET /fb?off=N[&w=W]    -> up to 8192 bytes of the thumbnail's RGB565
     *                            pixel stream starting at byte offset N
     * Self-contained: builds the WHOLE response into `out` (with a CORS
     * header) and returns here, so the aice-avm desktop page served from the
     * Mac can fetch it cross-origin and mirror the Pi 5 HDMI in a window. */
    if (path_eq(req, "/fb")) {
        extern const volatile unsigned char *video_fb_base(void);
        extern unsigned int video_fb_pitch(void);
        extern unsigned int video_screen_width(void), video_screen_height(void);

        unsigned srcw = video_screen_width(), srch = video_screen_height();
        const volatile unsigned char *fb = video_fb_base();
        unsigned pitch = video_fb_pitch();

        int dw = q_int(req, "w", 160);
        if (dw < 16)  dw = 16;
        if (dw > 480) dw = 480;
        int dh = (srcw && srch) ? (int)((unsigned)dw * srch / srcw) : (dw * 3 / 4);
        if (dh < 1) dh = 1;

        char offbuf[12];
        int has_off = q_param(req, "off", offbuf, sizeof offbuf);

        static char fbody[8192];   /* was 1100: one segment was all we could send */
        int fl = 0;
        const char *fbctype;
        if (!has_off) {                              /* /fb or /fb?w= -> dims */
            fbctype = "text/plain";
            fl = s_putdec(fbody, fl, dw);          fl = s_put(fbody, fl, " ");
            fl = s_putdec(fbody, fl, dh);          fl = s_put(fbody, fl, " ");
            fl = s_putdec(fbody, fl, (long)srcw);  fl = s_put(fbody, fl, " ");
            fl = s_putdec(fbody, fl, (long)srch);  fl = s_put(fbody, fl, "\n");
        } else {                                     /* /fb?off=N -> RGB565 chunk */
            fbctype = "application/octet-stream";
            int total = dw * dh * 2;
            int off = q_int(req, "off", 0);
            if (off < 0) off = 0;
            if (off & 1) off++;
            /* Was 1024, because the send path could only emit one segment.
             * With segmentation the browser needs 8x fewer round trips for
             * the same frame — and this is the regression test for it. */
            int chunk = 8192;
            if (off + chunk > total) chunk = total - off;
            if (chunk < 0) chunk = 0;
            if (fb && pitch) {
                int p0 = off / 2, np = chunk / 2;
                for (int i = 0; i < np; i++) {
                    int pi = p0 + i, ox = pi % dw, oy = pi / dw;
                    unsigned sx = (unsigned)ox * srcw / (unsigned)dw;
                    unsigned sy = (unsigned)oy * srch / (unsigned)dh;
                    unsigned px = *(const volatile unsigned int *)(fb + sy * pitch + sx * 4);
                    unsigned r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
                    unsigned v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                    fbody[fl++] = (char)(v & 0xFF);
                    fbody[fl++] = (char)((v >> 8) & 0xFF);
                }
            }
        }

        int p = 0;
        p = s_put(out, p, "HTTP/1.0 200 OK\r\nContent-Type: ");
        p = s_put(out, p, fbctype);
        p = s_put(out, p, "\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: ");
        p = s_putdec(out, p, fl);
        p = s_put(out, p, "\r\n\r\n");
        for (int i = 0; i < fl; i++) out[p++] = fbody[i];
        return p;
    }

    if (path_eq(req, "/send")) {
        int  to  = q_int(req, "to", -1);
        int  arg = q_int(req, "arg", 0);
        char method[ACTOR_NAMELEN];
        if (!q_param(req, "m", method, sizeof method)) method[0] = 0;
        int result = 0;
        int rc = actor_message(to, method, arg, &result);
        bl = s_put(body, bl, "{\"ok\":");
        bl = s_putdec(body, bl, rc == 0 ? 1 : 0);
        bl = s_put(body, bl, ",\"actor\":");
        bl = s_putdec(body, bl, to);
        bl = s_put(body, bl, ",\"method\":\"");
        bl = s_put(body, bl, method);
        bl = s_put(body, bl, "\",\"result\":");
        bl = s_putdec(body, bl, result);
        bl = s_put(body, bl, "}\n");
    } else if (path_eq(req, "/basic/run")) {
        /* Queue a BASIC direct command for the on-screen BASIC window, e.g.
         *   GET /basic/run?line=rescue
         * It runs from the wm tick (not here), so this returns immediately and
         * a long RUN does not wedge the HTTP server. */
        extern void basicwin_post_line(const char *);
        extern int  basicwin_running(void);
        char line[80];
        if (!q_param(req, "line", line, sizeof line)) line[0] = 0;
        /* URL-decode in place: %XX -> byte, '+' -> space.  Lets a command with
         * spaces/quotes survive q_param (which stops at a raw space), e.g.
         *   /basic/run?line=RUN%20%22rescue%22   ->   RUN "rescue" */
        {
            char *s = line, *d = line;
            while (*s) {
                if (*s == '%' && s[1] && s[2]) {
                    int hi = s[1], lo = s[2];
                    hi = (hi<='9')?hi-'0':((hi|0x20)-'a'+10);
                    lo = (lo<='9')?lo-'0':((lo|0x20)-'a'+10);
                    if (hi>=0 && hi<16 && lo>=0 && lo<16) { *d++ = (char)((hi<<4)|lo); s += 3; continue; }
                }
                *d++ = (*s == '+') ? ' ' : *s;
                s++;
            }
            *d = 0;
        }
        if (line[0]) basicwin_post_line(line);
        bl = s_put(body, bl, "{\"ok\":");
        bl = s_putdec(body, bl, line[0] ? 1 : 0);
        bl = s_put(body, bl, ",\"queued\":\"");
        bl = s_put(body, bl, line);
        bl = s_put(body, bl, "\",\"running\":");
        bl = s_putdec(body, bl, basicwin_running());
        bl = s_put(body, bl, "}\n");
    } else if (path_eq(req, "/basic/key")) {
        /* Inject a key into the BASIC window, e.g. GET /basic/key?c=3 (Ctrl-C).
         * Delivered even mid-RUN: the 100 Hz timer IRQ runs this handler. */
        extern void basicwin_inject_key(char);
        extern int  basicwin_running(void);
        int c = q_int(req, "c", 0);
        if (c) basicwin_inject_key((char)c);
        bl = s_put(body, bl, "{\"ok\":1,\"sent\":");
        bl = s_putdec(body, bl, c);
        bl = s_put(body, bl, ",\"running\":");
        bl = s_putdec(body, bl, basicwin_running());
        bl = s_put(body, bl, "}\n");
    } else if (path_eq(req, "/basic/break")) {
        /* Convenience: same as /basic/key?c=3 — request a Ctrl-C break. */
        extern void basicwin_inject_key(char);
        extern int  basicwin_running(void);
        basicwin_inject_key((char)0x03);
        bl = s_put(body, bl, "{\"ok\":1,\"running\":");
        bl = s_putdec(body, bl, basicwin_running());
        bl = s_put(body, bl, "}\n");
    } else if (path_eq(req, "/basic/state")) {
        extern int basicwin_running(void);
        bl = s_put(body, bl, "{\"running\":");
        bl = s_putdec(body, bl, basicwin_running());
        bl = s_put(body, bl, "}\n");
    } else if (path_eq(req, "/api/load")) {
        /* Node load metric for the mesh load-balancer's placement policy:
         * managers (Mac/Windows) poll this to pick the least-loaded Xinu node. */
        extern int cc_actor_live_count(void), cc_actor_capacity(void);
        ctype = "application/json";
        int live = cc_actor_live_count(), cap = cc_actor_capacity();
        int pct = cap > 0 ? (live * 100) / cap : 0;
        bl = s_put(body, bl, "{\"node\":\"xinu\",\"live_actors\":");
        bl = s_putdec(body, bl, live);
        bl = s_put(body, bl, ",\"capacity\":");
        bl = s_putdec(body, bl, cap);
        bl = s_put(body, bl, ",\"load_pct\":");
        bl = s_putdec(body, bl, pct);
        bl = s_put(body, bl, "}\n");
    } else if (path_eq(req, "/api/actors")) {
        int n = actor_count();
        bl = s_put(body, bl, "{\"actors\":[");
        for (int i = 0; i < n; i++) {
            if (i) bl = s_put(body, bl, ",");
            bl = s_put(body, bl, "{\"id\":");
            bl = s_putdec(body, bl, i);
            bl = s_put(body, bl, ",\"name\":\"");
            bl = s_put(body, bl, actor_name(i));
            bl = s_put(body, bl, "\",\"value\":");
            bl = s_putdec(body, bl, actor_field(i, 0));
            bl = s_put(body, bl, ",\"msgs\":");
            bl = s_putdec(body, bl, actor_field(i, 1));
            bl = s_put(body, bl, "}");
        }
        bl = s_put(body, bl, "]}\n");
    } else if (str_starts(rpath, "/microsd/write/")) {
        /* Persistent FAT32 write to the microSD root: POST /microsd/write/<NAME>
         * with the file content as the request body (8.3 name, <= one cluster). */
        extern int microsd_write_file(const char *name, const void *data, unsigned long len);
        ctype = "text/plain";
        const char *name = rpath + 15;          /* strlen("/microsd/write/") */
        const char *b = http_body(req);
        if (name[0] && b) {
            int blen = 0; while (b[blen]) blen++;
            int r = microsd_write_file(name, b, (unsigned long)blen);
            bl = s_put(body, bl, r == 0 ? "microsd write: ok " : "microsd write: FAILED ");
            bl = s_putdec(body, bl, blen); bl = s_put(body, bl, " bytes -> "); bl = s_put(body, bl, name);
            bl = s_put(body, bl, "\n");
        } else bl = s_put(body, bl, "microsd write: bad name or empty body\n");
    } else if (str_starts(rpath, "/fs")) {
        /* Hierarchical in-memory filesystem (fs/vfs.c) over HTTP:
         *   GET  /fs   or  /fs/tree        -> indented tree of the whole FS
         *   GET  /fs/ls/<dir>              -> one directory's entries
         *   GET  /fs/cat/<file>            -> a file's contents
         *   POST /fs/mkdir/<dir>           -> create dir + intermediates
         *   POST /fs/write/<file> (body)   -> create file + write the body  */
        ctype = "text/plain";
        if (str_starts(rpath, "/fs/cat/")) {
            vfs_node_t *n = vfs_lookup(rpath + 7);
            if (n && n->kind == VFS_FILE) bl = vfs_read(n, body, sizeof body - 1);
            else bl = s_put(body, bl, "cat: no such file\n");
            if (bl < 0) bl = 0;
        } else if (str_starts(rpath, "/fs/ls/")) {
            vfs_node_t *n = vfs_lookup(rpath + 6);
            if (n && n->kind == VFS_DIR) {
                for (vfs_node_t *c = n->children; c && bl < (int)sizeof body - 40; c = c->next) {
                    bl = s_put(body, bl, c->name);
                    bl = s_put(body, bl, c->kind == VFS_DIR ? "/\n" : "\n");
                }
                if (!n->children) bl = s_put(body, bl, "(empty)\n");
            } else bl = s_put(body, bl, "ls: no such directory\n");
        } else if (str_starts(rpath, "/fs/mkdir/")) {
            vfs_node_t *n = vfs_mkdir_p(rpath + 9);
            bl = s_put(body, bl, n ? "mkdir: ok\n" : "mkdir: failed\n");
        } else if (str_starts(rpath, "/fs/write/")) {
            vfs_node_t *n = vfs_create_path(rpath + 9);
            const char *b = http_body(req);
            if (n && n->kind == VFS_FILE && b) {
                int blen = 0; while (b[blen]) blen++;
                vfs_write(n, b, (unsigned long)blen);
                bl = s_put(body, bl, "write: "); bl = s_putdec(body, bl, blen); bl = s_put(body, bl, " bytes\n");
            } else bl = s_put(body, bl, "write: failed (bad path or empty body)\n");
        } else {                              /* /fs or /fs/tree */
            struct treectx t; t.b = body; t.bl = bl; t.max = (int)sizeof body;
            vfs_walk(vfs_root(), 0, tree_visit, &t);
            bl = t.bl;
        }
    } else if ((req[0]=='P'&&req[1]=='O'&&req[2]=='S'&&req[3]=='T'&&
                req[5]=='/'&&req[6]=='c'&&req[7]=='c') ||
               (req[0]=='G'&&req[1]=='E'&&req[2]=='T'&&
                req[4]=='/'&&req[5]=='c'&&req[6]=='c')) {
        /* C JIT: POST /cc with C source as the body -> compile to AArch64,
         * run main() in place, return its output + return value. */
        extern int cc_run_source(const char *src, int srclen,
                                 char *out, int outcap, long *retval);
        ctype = "text/plain";
        /* Body starts after the blank line (CRLFCRLF). */
        const char *bodyp = 0; int reqn = 0;
        while (req[reqn]) reqn++;
        for (int i = 0; i + 3 < reqn; i++)
            if (req[i]=='\r'&&req[i+1]=='\n'&&req[i+2]=='\r'&&req[i+3]=='\n') { bodyp = req + i + 4; break; }
        if (!bodyp || !*bodyp) {
            bl = s_put(body, bl, "usage: curl --data-binary @prog.c http://<ip>/cc\n"
                                 "C subset: int/char/ptr/array, +-*/%, if/while/for, "
                                 "functions, recursion; builtins print/putchar/puts/actor_send.\n");
        } else {
            int srclen = 0; while (bodyp[srclen]) srclen++;
            /* 本文が AIPL（最初の語が "class"）なら、機内の abcl2c で C に
               直してから JIT に渡す。シェルの cc/make が .abcl を透過的に
               扱うのと同じ経路を HTTP からも使えるようにする。

               判定の前に空白とコメントを読み飛ばす。以前は本文の先頭が
               literal に "class " のときしか見ていなかったので、
               正典のガイド標本のように "// g1: …" で始まる AIPL が
               C として扱われ、cc が "expected type" で落ちていた。
               読み飛ばすのは判定のためだけで、abcl2c には本文をそのまま渡す
               （abcl2c の字句解析器がコメントを扱う）。 */
            const char *runsrc = bodyp;
            int xlat_ok = 1;
            const char *hd = bodyp;
            for (;;) {
                while (*hd==' '||*hd=='\t'||*hd=='\r'||*hd=='\n') hd++;
                if (hd[0]=='/'&&hd[1]=='/') { hd+=2; while (*hd && *hd!='\n') hd++; continue; }
                if (hd[0]=='/'&&hd[1]=='*') { hd+=2; while (*hd && !(hd[0]=='*'&&hd[1]=='/')) hd++; if(*hd) hd+=2; continue; }
                break;
            }
            if (hd[0]=='c'&&hd[1]=='l'&&hd[2]=='a'&&hd[3]=='s'&&hd[4]=='s'&&
                (hd[5]==' '||hd[5]=='\t'||hd[5]=='\r'||hd[5]=='\n')) {
                extern int         abcl2c(const char *, int, char *, int);
                extern const char *abcl2c_error(void);
                static char xlat[32768];
                int xr = abcl2c(bodyp, srclen, xlat, (int)sizeof xlat);
                if (xr < 0) {
                    bl = s_put(body, bl, "abcl2c: ");
                    bl = s_put(body, bl, abcl2c_error());
                    bl = s_put(body, bl, "\n");
                    xlat_ok = 0;
                } else { runsrc = xlat; srclen = xr; }
            }
            if (xlat_ok) {
                static char ccout[1024];
                long rv = 0;
                /* ★ 専用プロセスで走らせる。以前はこの場（HTTP を処理している
                   文脈）で cc_run_source を直に呼んでいたので、wait() が使えず
                   g7 だけシェル経路と食い違っていた。
                   ここで眠るわけにはいかない —— HTTP の処理は net_proc からも
                   wm ループ（NULLPROC）からも入りうるし、NULLPROC で
                   proc_sleep_us すると ready が空なら自分自身へ ctxsw して眠らない。
                   代わりにシェルの cc と同じ「別プロセス・静的スタックで走らせて
                   消えるまで回す」形にする。中身は cc_run_source そのものなので、
                   常駐（web_expose のルート、/api/actors）の扱いは変わらない。 */
                extern int cc_run_source_proc(const char *, int, char *, int, long *);
                int rc = cc_run_source_proc(runsrc, srclen, ccout, (int)sizeof ccout, &rv);
                bl = s_put(body, bl, ccout);
                if (rc == 0) { bl = s_put(body, bl, "=> "); bl = s_putdec(body, bl, rv); bl = s_put(body, bl, "\n"); }
            }
        }
    } else if (str_starts(rpath, "/actor/loadvm")) {
        /* Upload an AIPL .avm actor module and run it in the kernel AVM VM (the
         * "Blender" polygon display).  Checked BEFORE /actor/load (which would
         * prefix-match "loadvm").  Chunked (g_req is 16 KB, so keep chunks
         * <= ~12 KB):
         *   POST /actor/loadvm?off=<byte>   body = a chunk of the .avm
         *   GET  /actor/loadvm?go=1&len=<N>  load + spawn + run -> VM gfx window */
        extern void avm_stage_reset(void);
        extern int  avm_stage_put(int, const unsigned char *, int);
        extern void avm_load_progress(int, int);
        extern int  avm_loadrun(int);
        ctype = "text/plain";
        if (req[0] == 'P') {                       /* POST a chunk */
            int off = q_int(req, "off", -1);
            int cl  = content_length(req);
            int tot = q_int(req, "total", 0);      /* for the on-screen bar */
            const char *bp = http_body(req);
            if (off == 0) avm_stage_reset();       /* new upload */
            if (off >= 0 && cl > 0 && bp) {
                int rc = avm_stage_put(off, (const unsigned char *)bp, cl);
                avm_load_progress(off + cl, tot);
                bl = s_put(body, bl, rc == 0 ? "ok off=" : "ERR off=");
                bl = s_putdec(body, bl, off);
                bl = s_put(body, bl, " n="); bl = s_putdec(body, bl, cl);
                bl = s_put(body, bl, "\n");
            } else {
                bl = s_put(body, bl, "usage: POST /actor/loadvm?off=<byte> body=<chunk>\n");
            }
        } else if (q_int(req, "go", 0)) {          /* GET ?go=1&len=N[&save=NAME] -> run */
            int len = q_int(req, "len", 0);
            char savename[16];
            if (q_param(req, "save", savename, sizeof savename) && savename[0]) {
                extern int avm_save(const char *, int);     /* RAM-resident store */
                extern int avm_save_sd(const char *, int);  /* best-effort microSD */
                int rc  = avm_save(savename, len);
                int sdrc = avm_save_sd(savename, len);
                bl = s_put(body, bl, "saved RAM="); bl = s_putdec(body, bl, rc);
                bl = s_put(body, bl, " SD="); bl = s_putdec(body, bl, sdrc);
                bl = s_put(body, bl, " name="); bl = s_put(body, bl, savename);
                bl = s_put(body, bl, "\n");
            }
            int id  = avm_loadrun(len);
            bl = s_put(body, bl, "loadvm: len="); bl = s_putdec(body, bl, len);
            bl = s_put(body, bl, " spawned actor id="); bl = s_putdec(body, bl, id);
            bl = s_put(body, bl, "\n");
        } else {
            bl = s_put(body, bl, "POST chunks to ?off=, then GET /actor/loadvm?go=1&len=<N>\n");
        }
    } else if (str_starts(rpath, "/actor/load")) {
        /* AIPL: POST the --xinu-jit C of an actor program; JIT it, spawn the
         * actors, run the initial message cascade, keep them resident. */
        extern int cc_actor_load(const char *src, int srclen, char *out, int outcap);
        ctype = "text/plain";
        const char *b = http_body(req);
        if (b && *b) { int n=0; while(b[n])n++; cc_actor_load(b, n, body, (int)sizeof body); bl = 0; while(body[bl])bl++; }
        else bl = s_put(body, bl, "usage: curl --data-binary @prog.c -X POST http://<ip>/actor/load\n");
    } else if (path_eq(req, "/actor/send")) {
        /* Message a resident actor: ?to=<id>&m=<method>&arg=<n>. */
        extern int cc_actor_send_msg(int to, const char *method, int arg, char *out, int outcap);
        ctype = "text/plain";
        int to = q_int(req, "to", 0);
        int arg = q_int(req, "arg", 0);
        char m[ACTOR_NAMELEN]; if (!q_param(req, "m", m, sizeof m)) m[0]=0;
        cc_actor_send_msg(to, m, arg, body, (int)sizeof body); bl = 0; while(body[bl])bl++;
    } else if (str_starts(rpath, "/api/x/")) {
        /* AIPL の web_expose("/echo", "echo") で公開したアクターへの入口。
             GET /api/x/<path>?method=<名前>&args=<文字列>
           <path> は web_expose の第1引数（先頭の '/' を含む）。
           この装置ではポート指定（web_listen の引数）は無視され、公開ルートは
           この 80 番のサーバに載る。 */
        extern int  cc_web_lookup(const char *path);
        extern int  cc_actor_send_msg_str(int to, const char *method,
                                          const char *sarg, char *out, int outcap);
        extern int  cc_web_count(void);
        extern const char *cc_web_path_at(int i);
        extern int  cc_web_actor_at(int i);
        ctype = "text/plain";
        const char *sub = rpath + 6;               /* strlen("/api/x") — '/' を残す */
        if (!sub[0] || (sub[0]=='/' && !sub[1])) {  /* 一覧を出す */
            bl = s_put(body, bl, "exposed routes (web_expose):\n");
            int n = cc_web_count();
            if (!n) bl = s_put(body, bl, "  (none — web_expose したプログラムを載せてください)\n");
            for (int i = 0; i < n; i++) {
                bl = s_put(body, bl, "  /api/x"); bl = s_put(body, bl, cc_web_path_at(i));
                bl = s_put(body, bl, "  -> actor #");
                { char t[12]; int tn=0,v=cc_web_actor_at(i); if(!v)t[tn++]='0';
                  while(v){t[tn++]=(char)('0'+v%10);v/=10;} while(tn) body[bl++]=t[--tn]; }
                bl = s_put(body, bl, "\n");
            }
        } else {
            int to = cc_web_lookup(sub);
            if (to < 0) {
                bl = s_put(body, bl, "no such exposed path: "); bl = s_put(body, bl, sub);
                bl = s_put(body, bl, "\n（GET /api/x/ で一覧）\n");
            } else {
                char m[ACTOR_NAMELEN]; if (!q_param(req, "method", m, sizeof m)) m[0]=0;
                char a[192];           if (!q_param(req, "args",   a, sizeof a)) a[0]=0;
                if (!m[0]) {
                    bl = s_put(body, bl, "usage: /api/x/<path>?method=<name>&args=<text>\n");
                } else {
                    cc_actor_send_msg_str(to, m, a, body, (int)sizeof body);
                    bl = 0; while (body[bl]) bl++;
                }
            }
        }
    } else if (str_starts(rpath, "/run")) {
        /* Run a shell command from HTTP — a reliable way to drive the shell
         * (wine/4lines/kodama/clear/...) when the USB keyboard is being flaky. */
        extern int shell_dispatch_line(char *line);
        extern void uart_capture(char *, int); extern void uart_capture_stop(void);
        ctype = "text/plain";
        char cmd[128];
        if (q_param(req, "cmd", cmd, sizeof cmd)) {
            /* URL-decode in place: %XX -> byte, '+' -> space. */
            int r = 0, w = 0;
            while (cmd[r]) {
                if (cmd[r] == '%' && cmd[r+1] && cmd[r+2]) {
                    int hi = cmd[r+1], lo = cmd[r+2];
                    hi = (hi>='0'&&hi<='9')?hi-'0':(hi|32)-'a'+10;
                    lo = (lo>='0'&&lo<='9')?lo-'0':(lo|32)-'a'+10;
                    cmd[w++] = (char)((hi<<4)|lo); r += 3;
                } else if (cmd[r] == '+') { cmd[w++] = ' '; r++; }
                else cmd[w++] = cmd[r++];
            }
            cmd[w] = 0;
            uart_capture(body, (int)sizeof body - 1);  /* tee command output into the reply */
            shell_dispatch_line(cmd);
            uart_capture_stop();
            bl = 0; while (body[bl]) bl++;              /* length of captured output */
        } else {
            bl = s_put(body, bl, "usage: /run?cmd=<shell command>\n");
        }
    } else if (str_starts(rpath, "/win/dump")) {
        /* Dump every window's current geometry so the live (dragged/resized)
         * layout can be captured back into the boot defaults. */
        extern struct window *wm_nth(int id);
        ctype = "text/plain";
        for (int i = 0; i < 16; i++) {
            struct window *w = wm_nth(i);
            if (!w) break;
            bl = s_putdec(body, bl, i);
            bl = s_put(body, bl, " x="); bl = s_putdec(body, bl, w->x);
            bl = s_put(body, bl, " y="); bl = s_putdec(body, bl, w->y);
            bl = s_put(body, bl, " w="); bl = s_putdec(body, bl, w->width);
            bl = s_put(body, bl, " h="); bl = s_putdec(body, bl, w->height);
            bl = s_put(body, bl, " \""); bl = s_put(body, bl, w->title); bl = s_put(body, bl, "\"\n");
        }
    } else if (str_starts(rpath, "/usb")) {
        /* USB/xHCI diagnostics over HTTP (serial is unreliable on Pi 5). */
        extern unsigned int rp1usb_ver(int); extern int rp1usb_ports(int);
        extern unsigned int rp1usb_snpsid(int); extern unsigned int rp1usb_caplen(int);
        extern unsigned int rp1usb_portsc(int,int); extern int rp1usb_connected(void);
        extern unsigned int rp1usb_usbsts(void); extern int rp1usb_running(void);
        extern unsigned int rp1usb_enum_portsc(void); extern unsigned int rp1usb_enum_cc(void);
        extern unsigned int rp1usb_enum_slotid(void); extern unsigned int rp1usb_addr_cc(void);
        extern int rp1usb_ctx_stride(void);
        ctype = "text/plain";
        /* Re-triggerable bring-up steps (iterate without reflashing): */
        if (str_starts(rpath, "/usb/probe")) {
            extern void rp1usb_probe(void);
            rp1usb_probe();
            bl = s_put(body, bl, "probe done: hciver="); bl = s_putdec(body, bl, rp1usb_ver(0));
            bl = s_put(body, bl, " ports="); bl = s_putdec(body, bl, rp1usb_ports(0));
            bl = s_put(body, bl, " c0p1="); bl = s_putdec(body, bl, rp1usb_portsc(0,0));
            bl = s_put(body, bl, " c0p2="); bl = s_putdec(body, bl, rp1usb_portsc(0,1));
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/init")) {
            extern int rp1usb_xhci_init(void); extern void rp1usb_select_ctrl(int);
            rp1usb_select_ctrl(q_int(req,"ctrl",0));   /* 0=usb0, 1=usb1 */
            int r = rp1usb_xhci_init();
            bl = s_put(body, bl, "xhci_init ctrl="); bl = s_putdec(body, bl, q_int(req,"ctrl",0));
            bl = s_put(body, bl, " -> "); bl = s_putdec(body, bl, r);
            bl = s_put(body, bl, " USBSTS="); bl = s_putdec(body, bl, rp1usb_usbsts());
            bl = s_put(body, bl, " running="); bl = s_putdec(body, bl, rp1usb_running());
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/reset")) {
            extern int rp1usb_enum_slot(int);
            int p = q_int(req, "port", 2);
            int r = rp1usb_enum_slot(p);
            bl = s_put(body, bl, "reset+enableslot port="); bl = s_putdec(body, bl, p);
            bl = s_put(body, bl, " -> slot="); bl = s_putdec(body, bl, r);
            bl = s_put(body, bl, " cc="); bl = s_putdec(body, bl, rp1usb_enum_cc());
            bl = s_put(body, bl, " PORTSC="); bl = s_putdec(body, bl, rp1usb_enum_portsc());
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/addr")) {
            extern int rp1usb_address_device(int,int,int,int);
            int slot = q_int(req,"slot",1), port = q_int(req,"port",2);
            int speed = q_int(req,"speed",2), bsr = q_int(req,"bsr",0);
            rp1usb_address_device(slot, port, speed, bsr);
            bl = s_put(body, bl, "addr slot="); bl = s_putdec(body, bl, slot);
            bl = s_put(body, bl, " port="); bl = s_putdec(body, bl, port);
            bl = s_put(body, bl, " speed="); bl = s_putdec(body, bl, speed);
            bl = s_put(body, bl, " bsr="); bl = s_putdec(body, bl, bsr);
            bl = s_put(body, bl, " -> cc="); bl = s_putdec(body, bl, rp1usb_addr_cc());
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/desc")) {
            extern int rp1usb_get_descriptor(int,int,int,int);
            extern unsigned int rp1usb_desc_cc(void); extern unsigned int rp1usb_desc_len(void);
            extern unsigned int rp1usb_desc_byte(int);
            int slot=q_int(req,"slot",1), type=q_int(req,"type",1);
            int index=q_int(req,"index",0), len=q_int(req,"len",18);
            rp1usb_get_descriptor(slot, type, index, len);
            bl = s_put(body, bl, "desc cc="); bl = s_putdec(body, bl, rp1usb_desc_cc());
            bl = s_put(body, bl, " len=");    bl = s_putdec(body, bl, rp1usb_desc_len());
            bl = s_put(body, bl, " bytes:");
            int dl = (int)rp1usb_desc_len();
            for (int i=0;i<dl && i<64 && bl<700;i++){ bl=s_put(body,bl," "); bl=s_putdec(body,bl,rp1usb_desc_byte(i)); }
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/hidsetup")) {
            extern int rp1usb_hid_setup(int,int,int,int);
            extern unsigned int rp1usb_setcfg_cc(void), rp1usb_cfgep_cc(void), rp1usb_setproto_cc(void);
            int slot=q_int(req,"slot",2), port=q_int(req,"port",1);
            int speed=q_int(req,"speed",1), mps=q_int(req,"mps",8);
            rp1usb_hid_setup(slot, port, speed, mps);
            bl = s_put(body, bl, "hidsetup setcfgCC="); bl = s_putdec(body, bl, rp1usb_setcfg_cc());
            bl = s_put(body, bl, " cfgepCC=");  bl = s_putdec(body, bl, rp1usb_cfgep_cc());
            bl = s_put(body, bl, " setprotoCC="); bl = s_putdec(body, bl, rp1usb_setproto_cc());
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/mouse?")) {
            extern int rp1usb_poll_mouse(int);
            extern unsigned int rp1usb_mouse_byte(int);
            int slot=q_int(req,"slot",2);
            int n = rp1usb_poll_mouse(slot);
            bl = s_put(body, bl, "mouse report len="); bl = s_putdec(body, bl, n);
            bl = s_put(body, bl, " bytes:");
            for (int i=0;i<4;i++){ bl=s_put(body,bl," "); bl=s_putdec(body,bl,rp1usb_mouse_byte(i)); }
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/getreport")) {
            extern int rp1usb_get_report(int,int);
            extern unsigned int rp1usb_mouse_byte(int), rp1usb_grep_cc(void);
            int slot=q_int(req,"slot",1), len=q_int(req,"len",4);
            int n = rp1usb_get_report(slot, len);
            bl = s_put(body, bl, "getreport cc="); bl = s_putdec(body, bl, rp1usb_grep_cc());
            bl = s_put(body, bl, " len="); bl = s_putdec(body, bl, n);
            bl = s_put(body, bl, " bytes:");
            for (int i=0;i<4;i++){ bl=s_put(body,bl," "); bl=s_putdec(body,bl,rp1usb_mouse_byte(i)); }
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/automouse")) {
            extern int rp1usb_hid_autosetup_if(int,int,int,int);
            extern int rp1usb_auto_epaddr(void), rp1usb_auto_mps(void), rp1usb_auto_iface(void),
                       rp1usb_auto_dci(void), rp1usb_auto_proto(void), rp1usb_auto_interval(void);
            extern unsigned int rp1usb_cfgep_cc(void), rp1usb_setproto_cc(void);
            int slot=q_int(req,"slot",1), port=q_int(req,"port",1), speed=q_int(req,"speed",1);
            int wif=q_int(req,"iface",-1);
            int r = rp1usb_hid_autosetup_if(slot, port, speed, wif);
            bl = s_put(body, bl, "automouse r="); bl = s_putdec(body, bl, r);
            bl = s_put(body, bl, " epaddr="); bl = s_putdec(body, bl, rp1usb_auto_epaddr());
            bl = s_put(body, bl, " dci="); bl = s_putdec(body, bl, rp1usb_auto_dci());
            bl = s_put(body, bl, " mps="); bl = s_putdec(body, bl, rp1usb_auto_mps());
            bl = s_put(body, bl, " interval="); bl = s_putdec(body, bl, rp1usb_auto_interval());
            bl = s_put(body, bl, " iface="); bl = s_putdec(body, bl, rp1usb_auto_iface());
            bl = s_put(body, bl, " proto="); bl = s_putdec(body, bl, rp1usb_auto_proto());
            bl = s_put(body, bl, " cfgepCC="); bl = s_putdec(body, bl, rp1usb_cfgep_cc());
            bl = s_put(body, bl, " setprotoCC="); bl = s_putdec(body, bl, rp1usb_setproto_cc());
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/autostart")) {
            extern int rp1usb_autostart(void);
            extern int rp1usb_mouse_on(void), rp1usb_kbd_on(void);
            int n = rp1usb_autostart();    /* re-init both controllers + rebind mouse+kbd */
            bl = s_put(body, bl, "autostart bound="); bl = s_putdec(body, bl, n);
            bl = s_put(body, bl, " mouse="); bl = s_putdec(body, bl, rp1usb_mouse_on());
            bl = s_put(body, bl, " kbd="); bl = s_putdec(body, bl, rp1usb_kbd_on());
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/mousefull")) {
            extern int rp1usb_mouse_fullsetup(int), rp1usb_full_slot(void), rp1usb_full_speed(void);
            extern int rp1usb_auto_epaddr(void), rp1usb_auto_dci(void), rp1usb_auto_mps(void),
                       rp1usb_auto_iface(void), rp1usb_auto_proto(void);
            extern unsigned int rp1usb_cfgep_cc(void), rp1usb_setproto_cc(void);
            int port=q_int(req,"port",1);
            int r = rp1usb_mouse_fullsetup(port);
            bl = s_put(body, bl, "mousefull r="); bl = s_putdec(body, bl, r);
            bl = s_put(body, bl, " slot="); bl = s_putdec(body, bl, rp1usb_full_slot());
            bl = s_put(body, bl, " speed="); bl = s_putdec(body, bl, rp1usb_full_speed());
            bl = s_put(body, bl, " epaddr="); bl = s_putdec(body, bl, rp1usb_auto_epaddr());
            bl = s_put(body, bl, " dci="); bl = s_putdec(body, bl, rp1usb_auto_dci());
            bl = s_put(body, bl, " mps="); bl = s_putdec(body, bl, rp1usb_auto_mps());
            bl = s_put(body, bl, " iface="); bl = s_putdec(body, bl, rp1usb_auto_iface());
            bl = s_put(body, bl, " proto="); bl = s_putdec(body, bl, rp1usb_auto_proto());
            bl = s_put(body, bl, " cfgepCC="); bl = s_putdec(body, bl, rp1usb_cfgep_cc());
            bl = s_put(body, bl, " setprotoCC="); bl = s_putdec(body, bl, rp1usb_setproto_cc());
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/pollmode")) {
            extern void rp1usb_set_poll_mode(int); extern int rp1usb_poll_mode_get(void);
            rp1usb_set_poll_mode(q_int(req,"on",1));
            bl = s_put(body, bl, "poll_mode="); bl = s_putdec(body, bl, rp1usb_poll_mode_get());
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/msd-setup")) {
            extern int rp1usb_msd_fullsetup(int), rp1usb_msd_slot(void),
                       rp1usb_msd_in_ep(void), rp1usb_msd_out_ep(void),
                       rp1usb_msd_in_dci(void), rp1usb_msd_out_dci(void);
            extern unsigned int rp1usb_msd_setup_cc(void), rp1usb_msd_cfgep_cc(void);
            int r = rp1usb_msd_fullsetup(q_int(req,"port",2));   /* boot stick = c0p2 */
            bl = s_put(body, bl, "msd-setup r="); bl = s_putdec(body, bl, r);
            bl = s_put(body, bl, " slot="); bl = s_putdec(body, bl, rp1usb_msd_slot());
            bl = s_put(body, bl, " setcfgCC="); bl = s_putdec(body, bl, rp1usb_msd_setup_cc());
            bl = s_put(body, bl, " cfgepCC="); bl = s_putdec(body, bl, rp1usb_msd_cfgep_cc());
            bl = s_put(body, bl, " inEP="); bl = s_putdec(body, bl, rp1usb_msd_in_ep());
            bl = s_put(body, bl, " outEP="); bl = s_putdec(body, bl, rp1usb_msd_out_ep());
            bl = s_put(body, bl, " inDCI="); bl = s_putdec(body, bl, rp1usb_msd_in_dci());
            bl = s_put(body, bl, " outDCI="); bl = s_putdec(body, bl, rp1usb_msd_out_dci());
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/msd-inquiry")) {
            extern int rp1usb_msd_inquiry(void);
            extern unsigned int rp1usb_msd_csw_status(void), rp1usb_msd_data_byte(int);
            extern int rp1usb_msd_p_cbw(void), rp1usb_msd_p_data(void), rp1usb_msd_p_csw(void);
            extern unsigned int rp1usb_msd_p_cbw_cc(void), rp1usb_msd_p_data_cc(void), rp1usb_msd_p_csw_cc(void);
            int r = rp1usb_msd_inquiry();
            bl = s_put(body, bl, "inquiry r="); bl = s_putdec(body, bl, r);
            bl = s_put(body, bl, " cswStatus="); bl = s_putdec(body, bl, rp1usb_msd_csw_status());
            bl = s_put(body, bl, "\n CBW: n="); bl = s_putdec(body, bl, rp1usb_msd_p_cbw());
            bl = s_put(body, bl, " cc="); bl = s_putdec(body, bl, (int)rp1usb_msd_p_cbw_cc());
            bl = s_put(body, bl, " | DATA: n="); bl = s_putdec(body, bl, rp1usb_msd_p_data());
            bl = s_put(body, bl, " cc="); bl = s_putdec(body, bl, (int)rp1usb_msd_p_data_cc());
            bl = s_put(body, bl, " | CSW: n="); bl = s_putdec(body, bl, rp1usb_msd_p_csw());
            bl = s_put(body, bl, " cc="); bl = s_putdec(body, bl, (int)rp1usb_msd_p_csw_cc());
            bl = s_put(body, bl, "\n vendor=");    /* INQUIRY bytes 8..15 = vendor ASCII */
            for (int i=8;i<16;i++){ unsigned c=rp1usb_msd_data_byte(i); if(c>=32&&c<127) body[bl++]=(char)c; }
            bl = s_put(body, bl, " product=");   /* bytes 16..31 = product ASCII */
            for (int i=16;i<32;i++){ unsigned c=rp1usb_msd_data_byte(i); if(c>=32&&c<127) body[bl++]=(char)c; }
            bl = s_put(body, bl, " first8:");
            for (int i=0;i<8;i++){ bl=s_put(body,bl," "); bl=s_putdec(body,bl,rp1usb_msd_data_byte(i)); }
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/msd-epstate")) {
            extern int rp1usb_msd_inquiry(void);
            extern int rp1usb_msd_p_cbw(void); extern unsigned int rp1usb_msd_p_cbw_cc(void);
            extern unsigned int rp1usb_msd_slotstate(void), rp1usb_msd_out_epstate(void),
                       rp1usb_msd_in_epstate(void), rp1usb_msd_out_deqlo(void),
                       rp1usb_msd_in_deqlo(void), rp1usb_msd_out_ep1(void),
                       rp1usb_msd_usbsts(void), rp1usb_msd_portsc2(void);
            extern int rp1usb_msd_out_dci(void), rp1usb_msd_in_dci(void);
            int r = rp1usb_msd_inquiry();
            bl = s_put(body, bl, "epstate inq_r="); bl = s_putdec(body, bl, r);
            bl = s_put(body, bl, " cbw_n="); bl = s_putdec(body, bl, rp1usb_msd_p_cbw());
            bl = s_put(body, bl, " cbw_cc="); bl = s_putdec(body, bl, (int)rp1usb_msd_p_cbw_cc());
            bl = s_put(body, bl, "\n slotstate="); bl = s_putdec(body, bl, (int)rp1usb_msd_slotstate());
            bl = s_put(body, bl, " outDCI="); bl = s_putdec(body, bl, rp1usb_msd_out_dci());
            bl = s_put(body, bl, " outEPstate="); bl = s_putdec(body, bl, (int)rp1usb_msd_out_epstate());
            bl = s_put(body, bl, " outEP_dw1=0x"); bl = s_puthex(body, bl, rp1usb_msd_out_ep1());
            bl = s_put(body, bl, " outDeqLo=0x"); bl = s_puthex(body, bl, rp1usb_msd_out_deqlo());
            bl = s_put(body, bl, "\n inDCI="); bl = s_putdec(body, bl, rp1usb_msd_in_dci());
            bl = s_put(body, bl, " inEPstate="); bl = s_putdec(body, bl, (int)rp1usb_msd_in_epstate());
            bl = s_put(body, bl, " inDeqLo=0x"); bl = s_puthex(body, bl, rp1usb_msd_in_deqlo());
            bl = s_put(body, bl, "\n USBSTS=0x"); bl = s_puthex(body, bl, rp1usb_msd_usbsts());
            bl = s_put(body, bl, " PORTSC2=0x"); bl = s_puthex(body, bl, rp1usb_msd_portsc2());
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/msd-capacity")) {
            extern int rp1usb_msd_capacity(void);
            extern unsigned int rp1usb_msd_csw_status(void), rp1usb_msd_blocks(void), rp1usb_msd_blocksize(void);
            int r = rp1usb_msd_capacity();
            bl = s_put(body, bl, "capacity r="); bl = s_putdec(body, bl, r);
            bl = s_put(body, bl, " cswStatus="); bl = s_putdec(body, bl, rp1usb_msd_csw_status());
            bl = s_put(body, bl, " blocks="); bl = s_putdec(body, bl, (int)rp1usb_msd_blocks());
            bl = s_put(body, bl, " blocksize="); bl = s_putdec(body, bl, (int)rp1usb_msd_blocksize());
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/msd-read")) {
            extern int rp1usb_msd_read_block(unsigned int);
            extern unsigned int rp1usb_msd_csw_status(void), rp1usb_msd_data_byte(int);
            unsigned int lba = (unsigned int)q_int(req,"lba",0);
            int r = rp1usb_msd_read_block(lba);
            bl = s_put(body, bl, "read lba="); bl = s_putdec(body, bl, (int)lba);
            bl = s_put(body, bl, " r="); bl = s_putdec(body, bl, r);
            bl = s_put(body, bl, " cswStatus="); bl = s_putdec(body, bl, rp1usb_msd_csw_status());
            bl = s_put(body, bl, " mbrSig=");    /* bytes 510,511 should be 85,170 (0x55AA) */
            bl = s_putdec(body, bl, rp1usb_msd_data_byte(510)); bl = s_put(body, bl, ",");
            bl = s_putdec(body, bl, rp1usb_msd_data_byte(511));
            bl = s_put(body, bl, " first16:");
            for (int i=0;i<16 && bl<680;i++){ bl=s_put(body,bl," "); bl=s_putdec(body,bl,rp1usb_msd_data_byte(i)); }
            bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/usb/msd-write")) {
            extern int rp1usb_msd_read_block(unsigned int), rp1usb_msd_write_block(unsigned int);
            extern void rp1usb_msd_fill_pattern(unsigned int);
            extern unsigned int rp1usb_msd_csw_status(void), rp1usb_msd_data_byte(int);
            unsigned int lba = (unsigned int)q_int(req,"lba",0);
            unsigned int seed = (unsigned int)q_int(req,"seed",0x41);
            rp1usb_msd_fill_pattern(seed);
            int w = rp1usb_msd_write_block(lba);
            unsigned int wstat = rp1usb_msd_csw_status();
            int rr = rp1usb_msd_read_block(lba);       /* read back to verify */
            bl = s_put(body, bl, "write lba="); bl = s_putdec(body, bl, (int)lba);
            bl = s_put(body, bl, " seed="); bl = s_putdec(body, bl, (int)seed);
            bl = s_put(body, bl, " w="); bl = s_putdec(body, bl, w);
            bl = s_put(body, bl, " wCSW="); bl = s_putdec(body, bl, (int)wstat);
            bl = s_put(body, bl, " readback r="); bl = s_putdec(body, bl, rr);
            bl = s_put(body, bl, " rCSW="); bl = s_putdec(body, bl, (int)rp1usb_msd_csw_status());
            bl = s_put(body, bl, " first4:");
            for (int i=0;i<4;i++){ bl=s_put(body,bl," "); bl=s_putdec(body,bl,rp1usb_msd_data_byte(i)); }
            bl = s_put(body, bl, " (expect "); bl = s_putdec(body, bl, (int)(seed&0xff));
            bl = s_put(body, bl, " "); bl = s_putdec(body, bl, (int)((seed+1)&0xff));
            bl = s_put(body, bl, " ...)\n");
        } else {
        bl = s_put(body, bl, "hciver=");   bl = s_putdec(body, bl, rp1usb_ver(0));
        bl = s_put(body, bl, " ports=");   bl = s_putdec(body, bl, rp1usb_ports(0));
        bl = s_put(body, bl, " caplen=");  bl = s_putdec(body, bl, rp1usb_caplen(0));
        bl = s_put(body, bl, " dwc3id=");  bl = s_putdec(body, bl, rp1usb_snpsid(0));
        bl = s_put(body, bl, " ctxstride="); bl = s_putdec(body, bl, rp1usb_ctx_stride());
        { extern unsigned int rp1usb_rtsoff(void), rp1usb_dboff(void), rp1usb_hccp1(void);
          bl = s_put(body, bl, " rtsoff="); bl = s_putdec(body, bl, rp1usb_rtsoff());
          bl = s_put(body, bl, " dboff=");  bl = s_putdec(body, bl, rp1usb_dboff());
          bl = s_put(body, bl, " hccp1=");  bl = s_putdec(body, bl, rp1usb_hccp1()); }
        bl = s_put(body, bl, "\nUSBSTS="); bl = s_putdec(body, bl, rp1usb_usbsts());
        bl = s_put(body, bl, " running="); bl = s_putdec(body, bl, rp1usb_running());
        bl = s_put(body, bl, " connected="); bl = s_putdec(body, bl, rp1usb_connected());
        bl = s_put(body, bl, "\nports c0: ");
        for (int p=0;p<3;p++){ bl=s_putdec(body,bl,rp1usb_portsc(0,p)); bl=s_put(body,bl," "); }
        bl = s_put(body, bl, "\nports c1: ");
        for (int p=0;p<3;p++){ bl=s_putdec(body,bl,rp1usb_portsc(1,p)); bl=s_put(body,bl," "); }
        bl = s_put(body, bl, "\nenum: resetPORTSC="); bl = s_putdec(body, bl, rp1usb_enum_portsc());
        bl = s_put(body, bl, " slotCC=");  bl = s_putdec(body, bl, rp1usb_enum_cc());
        bl = s_put(body, bl, " slot=");    bl = s_putdec(body, bl, rp1usb_enum_slotid());
        bl = s_put(body, bl, " addrDevCC="); bl = s_putdec(body, bl, rp1usb_addr_cc());
        { extern unsigned long rp1usb_mouse_reports(void);
          extern int rp1usb_mouse_on(void), rp1usb_last_btn(void), rp1usb_last_dx(void),
                     rp1usb_last_dy(void), rp1usb_evt_idx(void), rp1usb_ep1_idx_get(void);
          bl = s_put(body, bl, "\nmouse: active="); bl = s_putdec(body, bl, rp1usb_mouse_on());
          bl = s_put(body, bl, " reports="); bl = s_putdec(body, bl, (int)rp1usb_mouse_reports());
          bl = s_put(body, bl, " lastbtn="); bl = s_putdec(body, bl, rp1usb_last_btn());
          bl = s_put(body, bl, " lastdx="); bl = s_putdec(body, bl, rp1usb_last_dx());
          bl = s_put(body, bl, " lastdy="); bl = s_putdec(body, bl, rp1usb_last_dy());
          bl = s_put(body, bl, " evtidx="); bl = s_putdec(body, bl, rp1usb_evt_idx());
          bl = s_put(body, bl, " ep1idx="); bl = s_putdec(body, bl, rp1usb_ep1_idx_get()); }
        { extern int rp1usb_kbd_on(void); extern unsigned long rp1usb_kbd_reports(void);
          extern int rp1usb_mouse_ctrl_get(void), rp1usb_kbd_ctrl_get(void),
                     rp1usb_mouse_slot_get(void), rp1usb_mouse_dci_get(void),
                     rp1usb_kbd_slot_get(void), rp1usb_kbd_dci_get(void);
          bl = s_put(body, bl, "\nkbd: active="); bl = s_putdec(body, bl, rp1usb_kbd_on());
          bl = s_put(body, bl, " reports="); bl = s_putdec(body, bl, (int)rp1usb_kbd_reports());
          bl = s_put(body, bl, "\ntopo: mouse ctrl="); bl = s_putdec(body, bl, rp1usb_mouse_ctrl_get());
          bl = s_put(body, bl, " slot="); bl = s_putdec(body, bl, rp1usb_mouse_slot_get());
          bl = s_put(body, bl, " dci="); bl = s_putdec(body, bl, rp1usb_mouse_dci_get());
          bl = s_put(body, bl, " | kbd ctrl="); bl = s_putdec(body, bl, rp1usb_kbd_ctrl_get());
          bl = s_put(body, bl, " slot="); bl = s_putdec(body, bl, rp1usb_kbd_slot_get());
          bl = s_put(body, bl, " dci="); bl = s_putdec(body, bl, rp1usb_kbd_dci_get()); }
        { extern unsigned int rp1usb_slot_state(void), rp1usb_boundep_state(void),
                              rp1usb_boundep_deqlo(void), rp1usb_mfindex(void);
          extern int rp1usb_bound_dci(void);
          bl = s_put(body, bl, "\nepctx: slotstate="); bl = s_putdec(body, bl, rp1usb_slot_state());
          bl = s_put(body, bl, " bounddci="); bl = s_putdec(body, bl, rp1usb_bound_dci());
          bl = s_put(body, bl, " bstate="); bl = s_putdec(body, bl, rp1usb_boundep_state());
          bl = s_put(body, bl, " bdeqlo="); bl = s_putdec(body, bl, rp1usb_boundep_deqlo());
          bl = s_put(body, bl, " mfindex="); bl = s_putdec(body, bl, rp1usb_mfindex()); }
        bl = s_put(body, bl, "\n");
        }
    } else if (str_starts(rpath, "/api/actors-gc")) {
        /* GLOBAL actor GC: ?threshold_ms=<n>&dry=<0|1>.  Reaps idle actors. */
        extern int cc_actor_gc(long threshold_ms, int dry, char *out, int outcap);
        ctype = "text/plain";
        int th = q_int(req, "threshold_ms", 0);
        int dry = q_int(req, "dry", 0);
        cc_actor_gc((long)th, dry, body, (int)sizeof body); bl = 0; while(body[bl])bl++;
    } else if (path_eq(req, "/tcpstat")) {
        /* The counters `tcpstat` prints, over the wire.  This board has no
         * /shell route, so the serial-only command was unreadable remotely —
         * which meant tcp_conn_reap() could not be observed at all. */
        ctype = "text/plain";
        const char *st;
        switch (g_conn.state) {
            case TCP_LISTEN:      st = "LISTEN";      break;
            case TCP_SYN_RCVD:    st = "SYN_RCVD";    break;
            case TCP_ESTABLISHED: st = "ESTABLISHED"; break;
            case TCP_FIN_WAIT_1:  st = "FIN_WAIT_1";  break;
            case TCP_LAST_ACK:    st = "LAST_ACK";    break;
            default:              st = "CLOSED";      break;
        }
        bl = s_put(body, bl, "tcp state=");   bl = s_put(body, bl, st);
        bl = s_put(body, bl, " any=");        bl = s_putdec(body, bl, (long)g_tcp_any);
        bl = s_put(body, bl, " seg_rx=");     bl = s_putdec(body, bl, (long)g_seg_rx);
        bl = s_put(body, bl, " syn=");        bl = s_putdec(body, bl, (long)g_syn_seen);
        bl = s_put(body, bl, " synack=");     bl = s_putdec(body, bl, (long)g_synack_sent);
        bl = s_put(body, bl, " estab=");      bl = s_putdec(body, bl, (long)g_estab);
        bl = s_put(body, bl, " txfail=");     bl = s_putdec(body, bl, (long)g_txfail);
        bl = s_put(body, bl, " reaped=");     bl = s_putdec(body, bl, (long)g_reaped);
        bl = s_put(body, bl, " retrans=");    bl = s_putdec(body, bl, (long)g_retrans);
        bl = s_put(body, bl, " sndlen=");     bl = s_putdec(body, bl, (long)g_conn.snd_len);
        bl = s_put(body, bl, " una=");        bl = s_putdec(body, bl, (long)g_conn.snd_una);
        bl = s_put(body, bl, "\n  prev: len=");  bl = s_putdec(body, bl, g_snap_len);
        bl = s_put(body, bl, " nxt=");        bl = s_putdec(body, bl, g_snap_nxt);
        bl = s_put(body, bl, " una=");        bl = s_putdec(body, bl, g_snap_una);
        bl = s_put(body, bl, " win=");        bl = s_putdec(body, bl, g_snap_win);
        bl = s_put(body, bl, " state=");      bl = s_putdec(body, bl, g_snap_state);
        bl = s_put(body, bl, "\n  calls: pump=");  bl = s_putdec(body, bl, (long)g_pump_calls);
        bl = s_put(body, bl, " segs=");       bl = s_putdec(body, bl, (long)g_segs_tx);
        bl = s_put(body, bl, " ack=");        bl = s_putdec(body, bl, (long)g_ack_calls);
        bl = s_put(body, bl, " tick=");       bl = s_putdec(body, bl, (long)g_tick_calls);
        bl = s_put(body, bl, "\n");
    } else if (str_starts(rpath, "/chainload")) {
        /* Network kexec — POST the kernel in chunks to staging RAM, then GO.
         *   POST /chainload?off=<byte>  body = a chunk (<=8 KB)
         *   GET  /chainload?go=1&len=<N> -> relocate trampoline + boot it
         * RAM-only: a bad image just needs a real power-cycle (no SD write). */
        ctype = "text/plain";
        /* Staging comes from the heap, which is always above _end.  The fixed
         * 0x4000000 this used to use sits inside our own .bss (which has grown
         * to ~69 MB), so the upload overwrote the running kernel and the board
         * died mid-transfer — the same bug the Pi 4 had. */
        extern void *kmalloc(unsigned long);
        extern unsigned char _end[];
        if (req[0]=='P') {
            int off = q_int(req, "off", -1);
            const char *b = http_body(req);
            int cl = content_length(req);
            if (off == 0) {                                  /* new upload */
                g_chain_len = 0;
                if (!g_chain_stage) g_chain_stage = (unsigned char *)kmalloc(CHAIN_STAGE_CAP);
            }
            if (!g_chain_stage) {
                bl = s_put(body, bl, "chainload: no staging memory\n");
            } else if (off < 0 || !b || cl <= 0) {
                bl = s_put(body, bl, "usage: POST /chainload?off=<byte> body=<chunk>\n");
            } else if ((unsigned long)off + (unsigned long)cl > CHAIN_STAGE_CAP) {
                bl = s_put(body, bl, "chainload: chunk past staging capacity\n");
            } else {
                unsigned char *stage = g_chain_stage;
                for (int i = 0; i < cl; i++) stage[off + i] = (unsigned char)b[i];
                if (off + cl > g_chain_len) g_chain_len = off + cl;
                bl = s_put(body, bl, "ok off="); bl = s_putdec(body, bl, off);
                bl = s_put(body, bl, " n=");     bl = s_putdec(body, bl, cl);
                bl = s_put(body, bl, " total="); bl = s_putdec(body, bl, g_chain_len);
                bl = s_put(body, bl, "\n");
            }
        } else if (q_int(req, "go", 0)) {
            int len = q_int(req, "len", g_chain_len);
            extern void kernel_chainload(unsigned long, unsigned long);
            if (!g_chain_stage || len <= 0) {
                bl = s_put(body, bl, "chainload: nothing staged\n");
            } else if ((unsigned long)g_chain_stage < (unsigned long)_end) {
                bl = s_put(body, bl, "chainload: staging overlaps the running kernel; refused\n");
            } else {
                kernel_chainload((unsigned long)g_chain_stage,
                                 (unsigned long)len);        /* never returns */
            }
        } else {
            bl = s_put(body, bl, "staged="); bl = s_putdec(body, bl, g_chain_len);
            bl = s_put(body, bl, " bytes. GET /chainload?go=1&len=<N> to boot it\n");
        }
    } else if (str_starts(rpath, "/wifi")) {
        /* CYW43455 WiFi bring-up, driven over HTTP and brought up in stages
         * (the SDIO host layer is new; see device/wifi/wifi.c).  Each action
         * appends to the ~8 KB trace; retrieve it paginated via /wifi-trace. */
        ctype = "text/plain";
        if (str_starts(rpath, "/wifi-trace")) {
            int off = q_int(req, "off", 0);
            const char *t = wifi_trace(); int tl = wifi_trace_len();
            int i = 0;
            for (; off + i < tl && i < 600 && bl < (int)sizeof body - 1; i++)
                body[bl++] = t[off + i];
        } else if (str_starts(rpath, "/wifi-pinmux")) {
            int f = q_int(req, "fsel", -1);
            if (f >= 0) wifi_set_pin_fsel((unsigned int)f);
            wifi_pinmux_dump();
            bl = s_put(body, bl, "pinmux set; GET /wifi-trace?off=0 for the reg dump\n");
        } else if (str_starts(rpath, "/wifi-stage")) {
            int hz = q_int(req, "hz", 0); if (hz) wifi_set_fwload_hz((unsigned int)hz);
            int n  = q_int(req, "n", 0);
            int rc = wifi_probe_stage(n);
            bl = s_put(body, bl, "stage n="); bl = s_putdec(body, bl, n);
            bl = s_put(body, bl, " rc=");     bl = s_putdec(body, bl, rc);
            bl = s_put(body, bl, " tracelen="); bl = s_putdec(body, bl, wifi_trace_len());
            bl = s_put(body, bl, " -- GET /wifi-trace?off=0,600,1200,...\n");
        } else if (str_starts(rpath, "/wifi-bulk")) {
            int rc = wifi_probe_bulk(q_int(req, "kb", 64), (unsigned int)q_int(req, "hz", 0));
            bl = s_put(body, bl, "bulk rc="); bl = s_putdec(body, bl, rc); bl = s_put(body, bl, " -- see /wifi-trace\n");
        } else if (str_starts(rpath, "/wifi-win")) {
            int rc = wifi_probe_winwrite(q_int(req, "w", 0));
            bl = s_put(body, bl, "win rc="); bl = s_putdec(body, bl, rc); bl = s_put(body, bl, "\n");
        } else if (str_starts(rpath, "/wifi-scan")) {
            int rc = wifi_scan_run();
            bl = s_put(body, bl, "scan rc="); bl = s_putdec(body, bl, rc); bl = s_put(body, bl, " -- see /wifi-trace\n");
        } else if (str_starts(rpath, "/wifi-join")) {
            char ssid[40], pass[68];
            if (!q_param(req, "ssid", ssid, sizeof ssid)) ssid[0] = 0;
            if (!q_param(req, "pass", pass, sizeof pass)) pass[0] = 0;
            int rc = wifi_join_run(ssid, pass);
            bl = s_put(body, bl, "join rc="); bl = s_putdec(body, bl, rc); bl = s_put(body, bl, " -- see /wifi-trace\n");
        } else if (str_starts(rpath, "/wifi-dhcp")) {
            int rc = wifi_dhcp();
            bl = s_put(body, bl, "dhcp rc="); bl = s_putdec(body, bl, rc); bl = s_put(body, bl, " -- see /wifi-trace\n");
        } else if (str_starts(rpath, "/wifi-ping")) {
            char ip[20]; unsigned char a[4] = {0,0,0,0};
            if (q_param(req, "ip", ip, sizeof ip)) {
                int o = 0, v = 0, k = 0;
                for (; ip[o] && k < 4; o++) {
                    if (ip[o] == '.') { a[k++] = (unsigned char)v; v = 0; }
                    else if (ip[o] >= '0' && ip[o] <= '9') v = v*10 + (ip[o]-'0');
                }
                if (k < 4) a[k] = (unsigned char)v;
            }
            int rc = wifi_ping(a, q_int(req, "n", 4));
            bl = s_put(body, bl, "ping rc="); bl = s_putdec(body, bl, rc); bl = s_put(body, bl, " -- see /wifi-trace\n");
        } else if (str_starts(rpath, "/wifi-probe")) {
            int rc = wifi_probe();
            bl = s_put(body, bl, "probe rc="); bl = s_putdec(body, bl, rc); bl = s_put(body, bl, " -- see /wifi-trace\n");
        } else {
            bl = s_put(body, bl,
              "wifi routes (bring-up is staged; poll /wifi-trace for the log):\n"
              " /wifi-stage?n=-2..6[&hz=N]   (-2=power+host -1=pinmux+CMD5 0=chipid 1=ramscan\n"
              "                               2=halt 3=4KB 4=fwload 5=CR4 6=Fn2)\n"
              " /wifi-trace?off=N   /wifi-pinmux?fsel=N   /wifi-bulk?kb&hz   /wifi-win?w\n"
              " /wifi-scan   /wifi-join?ssid&pass   /wifi-dhcp   /wifi-ping?ip&n\n");
        }
    } else if (path_eq(req, "/smp-bench")) {
        /* SMP benchmark: count primes in [0,n) on 1 core, then on all online
         * cores, and report wall-clock times + speedup.  The two counts MUST
         * match (correctness check).  Query: ?n=<upper bound> (default 300000).
         *   curl 'http://192.168.3.101/smp-bench?n=300000' */
        ctype = "text/plain";
        long n = (long)q_int(req, "n", 300000);
        if (n < 1) n = 1;
        int online = smp_cores_online();

        unsigned long t0 = now_ms();
        long c1 = smp_parallel_sum(bench_count_primes, n, 1);
        unsigned long t1 = now_ms();
        long cN = smp_parallel_sum(bench_count_primes, n, online);
        unsigned long t2 = now_ms();

        unsigned long ms1 = t1 - t0;
        unsigned long msN = t2 - t1;
        bl = s_put(body, bl, "SMP prime-count benchmark\n");
        bl = s_put(body, bl, "cores_online = "); bl = s_putdec(body, bl, (long)online); bl = s_put(body, bl, "\n");
        bl = s_put(body, bl, "n            = "); bl = s_putdec(body, bl, n); bl = s_put(body, bl, "\n");
        bl = s_put(body, bl, "primes       = "); bl = s_putdec(body, bl, c1);
        bl = s_put(body, bl, (c1 == cN) ? " (match)\n" : " MISMATCH!\n");
        bl = s_put(body, bl, "1-core   ms  = "); bl = s_putdec(body, bl, (long)ms1); bl = s_put(body, bl, "\n");
        bl = s_put(body, bl, "N-core   ms  = "); bl = s_putdec(body, bl, (long)msN); bl = s_put(body, bl, "\n");
        bl = s_put(body, bl, "speedup x100 = ");
        bl = s_putdec(body, bl, msN ? (long)((ms1 * 100UL) / msN) : 0);
        bl = s_put(body, bl, "  (e.g. 385 = 3.85x)\n");
    } else if (path_eq(req, "/bench")) {
        /* Unified SMP benchmark: kind=nqueens|dining|primes.  Reports the SAME
         * 1-core vs N-core wall times + speedup fields as /smp-bench so the Mesh
         * Control Center tabulates them uniformly.
         *   curl 'http://192.168.3.101/bench?kind=nqueens&n=13'
         *   curl 'http://192.168.3.101/bench?kind=dining&n=5' */
        ctype = "text/plain";
        int online = smp_cores_online();
        char kind[16];
        if (!q_param(req, "kind", kind, sizeof kind)) { kind[0] = 'n'; kind[1] = 'q'; kind[2] = 0; }
        int is_dining = str_starts(kind, "dining");
        int is_primes = str_starts(kind, "primes");
        int is_fill   = str_starts(kind, "fill");
        int is_null   = str_starts(kind, "null");
        /* cores caps the parallel run for an Amdahl sweep; 0 = all online.
         * Matches Pi 3/Pi 4 /bench?cores=N so all three boards sweep alike. */
        int cores = q_int(req, "cores", 0);
        int nc = (cores >= 1 && cores <= online) ? cores : online;
        smp_range_fn fn;
        long units;
        const char *label, *metric;
        if (is_dining) {
            g_din_n = q_int(req, "n", 5);
            if (g_din_n < 2)  g_din_n = 2;
            if (g_din_n > 64) g_din_n = 64;
            units = 200000;            /* independent tables, split across cores */
            fn = bench_dining;  label = "dining"; metric = "meals";
        } else if (is_primes) {
            units = q_int(req, "n", 300000);
            if (units < 1) units = 1;
            fn = bench_count_primes;  label = "primes"; metric = "primes";
        } else if (is_fill) {
            units = q_int(req, "n", BENCH_FILL_WORDS);
            if (units < 1) units = 1;
            if (units > BENCH_FILL_WORDS) units = BENCH_FILL_WORDS;
            fn = bench_fill;  label = "fill"; metric = "words";
        } else if (is_null) {
            units = nc;
            fn = bench_null;  label = "null"; metric = "chunks";
        } else {
            g_nq_n = q_int(req, "n", 13);
            if (g_nq_n < 1)  g_nq_n = 1;
            if (g_nq_n > 15) g_nq_n = 15;
            units = g_nq_n;            /* first-queen columns, split across cores */
            fn = bench_nqueens;  label = "nqueens"; metric = "solutions";
        }

        /* Timed with now_us() (generic-timer microseconds), NOT now_ms():
         * fill is sub-millisecond.  Serial then parallel back-to-back so the
         * ratio is robust to clock changes — same discipline as Pi 3/Pi 4. */
        unsigned long t0 = now_us();
        long r1 = smp_parallel_sum(fn, units, 1);
        unsigned long us1 = now_us() - t0;
        t0 = now_us();
        long rN = smp_parallel_sum(fn, units, nc);
        unsigned long usN = now_us() - t0;

        bl = s_put(body, bl, "SMP bench kind="); bl = s_put(body, bl, label); bl = s_put(body, bl, "\n");
        bl = s_put(body, bl, "cores_online = "); bl = s_putdec(body, bl, (long)nc); bl = s_put(body, bl, "\n");
        bl = s_put(body, bl, metric); bl = s_put(body, bl, " = "); bl = s_putdec(body, bl, r1); bl = s_put(body, bl, "\n");
        bl = s_put(body, bl, "1-core   us  = "); bl = s_putdec(body, bl, (long)us1); bl = s_put(body, bl, "\n");
        bl = s_put(body, bl, "N-core   us  = "); bl = s_putdec(body, bl, (long)usN); bl = s_put(body, bl, "\n");
        bl = s_put(body, bl, "speedup x100 = ");
        bl = s_putdec(body, bl, usN ? (long)((us1 * 100UL) / usN) : 0);
        bl = s_put(body, bl, "\n");
        bl = s_put(body, bl, "agree = "); bl = s_put(body, bl, (r1 == rN) ? "yes\n" : "NO\n");
    } else if (path_eq(req, "/nqpart")) {
        /* Distributed N-Queens partial: count solutions for first-queen columns
         * [c0,c1) at board size n, using all cores.  A Mac orchestrator hands
         * each cluster node a disjoint column range and sums the partials.
         *   curl 'http://192.168.3.101/nqpart?n=13&c0=0&c1=4' */
        ctype = "text/plain";
        int online = smp_cores_online();
        g_nqp_n = q_int(req, "n", 13);
        if (g_nqp_n < 1)  g_nqp_n = 1;
        if (g_nqp_n > 16) g_nqp_n = 16;
        int c0 = q_int(req, "c0", 0);
        int c1 = q_int(req, "c1", g_nqp_n);
        if (c0 < 0) c0 = 0;
        if (c1 > g_nqp_n) c1 = g_nqp_n;
        if (c1 < c0) c1 = c0;
        g_nqp_c0 = c0;
        unsigned long t0 = now_ms();
        long sol = smp_parallel_sum(bench_nqueens_part, (long)(c1 - c0), online);
        unsigned long t1 = now_ms();
        bl = s_put(body, bl, "nqueens-partial n="); bl = s_putdec(body, bl, (long)g_nqp_n);
        bl = s_put(body, bl, " c0="); bl = s_putdec(body, bl, (long)c0);
        bl = s_put(body, bl, " c1="); bl = s_putdec(body, bl, (long)c1);
        bl = s_put(body, bl, " solutions="); bl = s_putdec(body, bl, sol);
        bl = s_put(body, bl, " ms="); bl = s_putdec(body, bl, (long)(t1 - t0));
        bl = s_put(body, bl, " cores="); bl = s_putdec(body, bl, (long)online);
        bl = s_put(body, bl, "\n");
    } else if (path_eq(req, "/")) {
        ctype = "text/plain";
        bl = s_put(body, bl, "xinu-rpi5 (Pi 5) actor HTTP gateway\n"
                             "GET /smp-bench?n=<N>  (4-core prime-count benchmark)\n"
                             "GET /api/actors\n"
                             "GET /send?to=<id>&m=<bump|add|set|get|reset>&arg=<n>\n"
                             "POST /cc  (C or AIPL source in body) -> JIT compile & run\n"
                             "GET /fs | /fs/ls/<dir> | /fs/cat/<file>\n"
                             "POST /fs/mkdir/<dir> | /fs/write/<file> (body)\n"
                             "POST /actor/load (AIPL --xinu-jit C) ; GET /actor/send?to&m&arg\n"
                             "GET /api/actors-gc?threshold_ms=<n>&dry=<0|1>  (global actor GC)\n");
    } else {
        ctype = "text/plain";
        bl = s_put(body, bl, "404 not found\n");
    }
    body[bl] = 0;

    /* Headers + body. */
    int p = 0;
    p = s_put(out, p, "HTTP/1.0 200 OK\r\nContent-Type: ");
    p = s_put(out, p, ctype);
    p = s_put(out, p, "\r\nConnection: close\r\nContent-Length: ");
    p = s_putdec(out, p, bl);
    p = s_put(out, p, "\r\n\r\n");
    for (int i = 0; i < bl; i++) out[p++] = body[i];
    return p;
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

    /* Both total_len and data_off come off the wire.  Validate them
     * against the frame we actually received before deriving data/data_len:
     * otherwise `data` can point past the frame (into stale RX-ring bytes,
     * i.e. previous packets) and the bogus data_len desyncs peer_seq. */
    if (data_off < 20) return 1;                     /* malformed header */
    if (total_len < ihl + data_off) return 1;
    if (total_len > len - 14) return 1;              /* header lies about length */

    int data_len  = total_len - ihl - data_off;
    const volatile unsigned char *data = tcp + data_off;

    g_conn.last_ms = now_ms();          /* keep-alive for tcp_conn_reap() */

    /* Peer's advertised receive window — tcp_pump() will not put more than
     * this in flight.  Previously ignored entirely. */
    g_conn.peer_win = (unsigned short)(((unsigned short)tcp[14] << 8) | tcp[15]);

    /* Slide the send window on any ACK for outstanding data, whatever state
     * we are in; tcp_pump() then emits the next segments. */
    if ((flags & TCP_FLAG_ACK) && g_conn.snd_len > 0) tcp_on_ack(ack);

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
        /* Keep the previous connection's send state for /tcpstat. */
        g_snap_len   = g_conn.snd_len;
        g_snap_nxt   = g_conn.snd_nxt;
        g_snap_una   = g_conn.snd_una;
        g_snap_win   = g_conn.peer_win;
        g_snap_state = g_conn.state;
        /* Fresh connection: nothing outstanding from the previous one. */
        g_conn.snd_len = g_conn.snd_una = g_conn.snd_nxt = 0;
        g_conn.snd_fin_pending = 0;
        g_conn.snd_retries     = 0;
        g_conn.peer_fin        = 0;
        g_conn.fin_acked       = 0;
        g_reqlen               = 0;

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
            g_reqlen = 0;                       /* fresh request accumulator */
            g_estab++;
            uart_puts("tcp: ESTABLISHED (await HTTP request)\n");
            /* HTTP: the client sends the request first; we reply when
             * its data arrives (see the ESTABLISHED data path). */
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
            /* Accumulate the HTTP request across TCP segments — a POST /cc or
             * /actor/load body (AIPL C) easily exceeds one MSS.  We only act
             * once http_complete() says headers + Content-Length bytes are in;
             * until then we just ACK and wait for the next segment. */
            static char g_req[65536];   /* 64 KB: bigger /actor/loadvm chunks (~60 KB) */
            /* Was 1400 — one segment's worth, because that is all the send
             * path could carry.  It now segments, so the cap is the send
             * buffer instead of the MTU. */
            static char http_resp[TCP_SND_BUF];

            /* Only accept in-order data (seq == what we expect); ignore the
             * rest so a retransmit can't corrupt the buffer. */
            if ((unsigned long)seq == g_conn.peer_seq) {
                for (int i = 0; i < data_len && g_reqlen < (int)sizeof(g_req) - 1; i++)
                    g_req[g_reqlen++] = (char)data[i];
                g_req[g_reqlen] = 0;
                g_conn.peer_seq = seq + data_len;
            }
            tcp_send(TCP_FLAG_ACK, 0, 0);          /* acknowledge the segment */

            if (http_complete(g_req, g_reqlen)) {
                int rlen = http_build(g_req, http_resp, (int)sizeof http_resp);
                g_reqlen = 0;
                /* Queue it: tcp_pump() segments, tracks what the peer has
                 * ACKed, and sends the FIN with the final segment. */
                tcp_send_response(http_resp, rlen);
                uart_puts("http: served request\n");
            }
            /* fall through: this segment may also carry FIN */
        }
        if (flags & TCP_FLAG_FIN) {
            g_conn.peer_seq = seq + 1;
            g_conn.peer_fin = 1;
            /* Half-close.  The peer's FIN means it will send no more — NOT
             * that it stopped reading.  Closing here threw away whatever the
             * response still had queued: a client that shuts down its write
             * side after the request (nc, and any HTTP/1.0 client that does
             * the same) got the transfer cut at the in-flight limit, with the
             * FIN consuming a sequence number that then made snd_una overtake
             * snd_nxt and wedged both the pump and the retransmit timer. */
            if (g_conn.snd_nxt < g_conn.snd_len || g_conn.snd_fin_pending) {
                tcp_send(TCP_FLAG_ACK, 0, 0);   /* ack the FIN, keep sending */
                tcp_pump();
                return 1;
            }
            tcp_send(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
            g_conn.my_seq += 1;
            g_conn.state = TCP_LAST_ACK;
            uart_puts("tcp: peer FIN, sent FIN+ACK\n");
            return 1;
        }
        return 1;
    }

    if (g_conn.state == TCP_FIN_WAIT_1) {
        /* A normal client sends the ACK of our FIN and its own FIN as two
         * SEPARATE segments.  This used to close only when one segment
         * carried both, so the single connection slot stayed in FIN_WAIT_1
         * until the 60 s reaper and the NEXT request was refused — one
         * request per minute.  Track the two events independently. */
        if ((flags & TCP_FLAG_ACK) && ack == g_conn.my_seq) g_conn.fin_acked = 1;
        if (flags & TCP_FLAG_FIN) {
            g_conn.peer_seq = seq + 1;
            g_conn.peer_fin = 1;
            tcp_send(TCP_FLAG_ACK, 0, 0);
        }
        /* With one slot, holding it for a TIME_WAIT we cannot afford costs
         * more than it buys: once our FIN is acknowledged the response is
         * delivered, so free the slot.  A late FIN retransmit then lands in
         * LISTEN, where non-SYN segments are ignored. */
        if (g_conn.fin_acked) {
            g_conn.state = TCP_LISTEN;
            uart_puts("tcp: closed (back to LISTEN)\n");
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
    uart_puts(" reaped=");       puts_u32_dec(g_reaped);
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
unsigned long tcp_reaped_count(void)  { return g_reaped;      }

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
