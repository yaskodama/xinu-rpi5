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
#include "cc.h"

/* On-device LLM (llm/llm.c): generate text into `out`, return token count. */
extern int llm_run(const char *prompt, int max_new, char *out, int outcap, int echo);
/* Send a STRING message to a resident actor's method; reply -> out (cc/cc.c). */
extern int cc_actor_send_str(int actor, const char *method, const char *strarg, char *out, int outcap);
/* 100 Hz IRQ-driven tick counter (device/timer/timer.c) — for /ticks diag. */
extern unsigned long timer_ticks(void);
/* Preemptive-scheduling demo (system/actorproc.c): interleave log -> out. */
extern int preempt_demo(char *out, int outcap);

/* GENET RX interrupt fire counter (device/genet/genet.c) — for /genetirq diag. */
extern unsigned long genet_irq_count(void);

/* wm window inventory (device/video/wm.c) — for the /windows layout designer. */
extern int wm_window_count(void);
extern int wm_window_get(int idx, int *x, int *y, int *w, int *h);
extern int wm_window_name(int idx, char *out, int cap);
extern int wm_window_fontscale(int idx);

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

/* HTTP request accumulator (reassembles a request that spans segments,
 * e.g. a /compile POST body).  Reset on each new ESTABLISHED connection. */
static char g_httpreq[8192];
static int  g_httpreqlen;

/* ---------- app-layer handoff (preemptive networking) ----------
 * The net process must NOT run http_build (cc/llm) inline, or a long
 * compute stalls RX/ICMP/TCP for everyone.  Instead a complete request is
 * handed to a dedicated app-worker process via this single-slot mailbox;
 * the net process keeps draining and later flushes the finished response.
 * GENET/TCP stays owned by the net process; vheap/cc/llm by the worker.
 * One in-flight request at a time (matches the single g_conn).
 *
 * State is owned by exactly one side at each step, so a plain volatile int
 * is race-free under preemption: net sets QUEUED (only from IDLE), worker
 * sets WORKING then DONE, net sets IDLE on flush. */
enum { APP_IDLE = 0, APP_QUEUED, APP_WORKING, APP_DONE };
static volatile int g_app_state;
static char         g_app_req[sizeof g_httpreq];
static int          g_app_req_len;
static char         g_app_resp[1500];
static int          g_app_resp_len;
static unsigned long g_app_served;     /* responses flushed (diagnostic)     */
static int          g_net_preempt;     /* preemptive networking on/off        */

/* App-worker heartbeat: bumped on each request and per LLM token (llm.c) so a
 * wedge is visible on the HDMI runtime monitor even when HTTP is dead — if
 * app_state == WORKING but the heartbeat stops advancing, the worker is stuck.
 * Read by the wm in NULLPROC (which never wedges), so the channel survives. */
volatile unsigned long g_app_heartbeat;
void          app_beat(void)          { g_app_heartbeat++; }
int           rt_app_state(void)      { return g_app_state; }
unsigned long rt_served(void)         { return g_app_served; }
unsigned long rt_heartbeat(void)      { return g_app_heartbeat; }

/* Preemption control (system/proc.c) — toggled at runtime via /netpreempt
 * so preemptive networking can be enabled only after correctness is proven. */
extern void proc_set_preempt(int on);

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

static int starts_with(const char *s, const char *p)
{
    while (*p) { if (*s != *p) return 0; s++; p++; }
    return 1;
}

/* Index just past the "\r\n\r\n" header terminator, or -1 if not present. */
static int find_header_end(const char *s)
{
    for (int i = 0; s[i]; i++)
        if (s[i]=='\r' && s[i+1]=='\n' && s[i+2]=='\r' && s[i+3]=='\n') return i + 4;
    return -1;
}

static int lc(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Value of the Content-Length header (case-insensitive), or -1. */
static int content_length(const char *s)
{
    const char *key = "content-length:";
    for (const char *p = s; *p; p++) {
        const char *k = key; const char *q = p;
        while (*k && lc(*q) == *k) { q++; k++; }
        if (*k == 0) {
            while (*q == ' ') q++;
            int n = 0, any = 0;
            while (*q >= '0' && *q <= '9') { n = n*10 + (*q - '0'); q++; any = 1; }
            return any ? n : -1;
        }
    }
    return -1;
}

/* Has the full HTTP request arrived?  GET = once headers end; POST = once
 * the Content-Length body has been received too. */
static int request_complete(const char *req, int len)
{
    int he = find_header_end(req);
    if (he < 0) return 0;
    if (starts_with(req, "POST")) {
        int cl = content_length(req);
        if (cl < 0) return 1;
        return (len - he) >= cl;
    }
    return 1;
}

/* URL-decode `in` into `out` (%XX and '+' -> space).  Returns out length. */
static int url_decode(const char *in, char *out, int max)
{
    int o = 0;
    for (int i = 0; in[i] && o < max - 1; i++) {
        char c = in[i];
        if (c == '+') { out[o++] = ' '; }
        else if (c == '%' && in[i+1] && in[i+2]) {
            int hi = in[i+1], lo = in[i+2];
            hi = (hi<='9')?hi-'0':(lc(hi)-'a'+10);
            lo = (lo<='9')?lo-'0':(lc(lo)-'a'+10);
            out[o++] = (char)((hi<<4)|lo); i += 2;
        } else out[o++] = c;
    }
    out[o] = 0;
    return o;
}

/* Build the HTTP response for NUL-terminated request `req` into `out`
 * (capacity `max`).  Returns the byte length. */
static int http_build(const char *req, char *out, int max)
{
    static char body[1400];
    int  bl = 0;
    const char *ctype = "application/json";
    (void)max;

    if (starts_with(req, "POST /compile") || starts_with(req, "GET /compile")) {
        /* Dynamic compilation: the request carries C source, which we
         * compile and JIT-run in place; the reply is the program output
         * plus "=> <return value>".  Source is the POST body, or the
         * url-encoded `src` query param for a GET. */
        ctype = "text/plain";
        static char src[7168];
        int slen = 0;
        if (req[0] == 'P') {
            int he = find_header_end(req);
            if (he >= 0) {
                const char *b = req + he;
                while (b[slen] && slen < (int)sizeof(src) - 1) { src[slen] = b[slen]; slen++; }
                src[slen] = 0;
            }
        } else {
            static char enc[7168];
            if (q_param(req, "src", enc, sizeof enc)) slen = url_decode(enc, src, sizeof src);
        }

        long rv = 0;
        static char prog[1100];
        int rc = cc_run_source(src, slen, prog, sizeof prog, &rv);
        bl = s_put(body, bl, prog);
        if (rc == 0) { bl = s_put(body, bl, "=> "); bl = s_putdec(body, bl, rv); bl = s_put(body, bl, "\n"); }
        else         { bl = s_put(body, bl, "\n"); }
    } else if (starts_with(req, "POST /llm") || starts_with(req, "GET /llm")) {
        /* On-device LLM: prompt = POST body (or ?p= for GET), ?n= caps the
         * token count.  Reply is the generated text.  Generation blocks this
         * tick (no RX draining here — we are inside genet_rx_tick), so the
         * token count is capped to bound the stall. */
        ctype = "text/plain";
        static char prompt[256];
        int pl = 0;
        if (req[0] == 'P') {
            int he = find_header_end(req);
            if (he >= 0) { const char *b = req + he;
                while (b[pl] && pl < (int)sizeof(prompt)-1) { prompt[pl] = b[pl]; pl++; } }
            prompt[pl] = 0;
        } else {
            static char enc[256];
            if (q_param(req, "p", enc, sizeof enc)) url_decode(enc, prompt, sizeof prompt);
        }
        int n = 32;
        { char nb[12]; if (q_param(req, "n", nb, sizeof nb)) {
            int v = 0; for (char *s = nb; *s >= '0' && *s <= '9'; s++) v = v*10 + (*s - '0');
            if (v > 0) n = v; } }
        if (n > 96) n = 96;
        static char ltxt[700];
        llm_run(prompt[0] ? prompt : (const char *)0, n, ltxt, sizeof ltxt, 1);  /* echo prompt */
        bl = s_put(body, bl, ltxt);
        bl = s_put(body, bl, "\n");
    } else if (starts_with(req, "GET /ticks") || starts_with(req, "POST /ticks")) {
        /* Diagnostic: is the 100 Hz timer IRQ actually firing?  tick_count is
         * advanced only by the timer ISR; cntpct is the free-running hardware
         * counter (always advances).  If ticks stays 0 across calls while
         * cntpct grows, the timer IRQ is not being delivered. */
        ctype = "text/plain";
        unsigned long ct, el, ctl, daif;
        __asm__ volatile ("mrs %0, cntpct_el0"  : "=r"(ct));
        __asm__ volatile ("mrs %0, currentel"   : "=r"(el));
        __asm__ volatile ("mrs %0, cntp_ctl_el0": "=r"(ctl));
        __asm__ volatile ("mrs %0, daif"        : "=r"(daif));
        bl = s_put(body, bl, "ticks=");      bl = s_putdec(body, bl, (long)timer_ticks());
        bl = s_put(body, bl, " cntpct=");    bl = s_putdec(body, bl, (long)ct);
        bl = s_put(body, bl, " EL=");        bl = s_putdec(body, bl, (long)((el >> 2) & 3));
        bl = s_put(body, bl, " cntp_ctl=");  bl = s_putdec(body, bl, (long)ctl);   /* bit0=EN bit1=IMASK bit2=ISTATUS */
        bl = s_put(body, bl, " daif=");      bl = s_putdec(body, bl, (long)((daif >> 6) & 0xf));
        bl = s_put(body, bl, "\n");
    } else if (starts_with(req, "GET /preempt") || starts_with(req, "POST /preempt")) {
        /* Demo: 2 CPU-bound procs time-sliced by the timer.  Cooperative ->
         * "AAAA...BBBB"; preemptive -> interleaved "ABABAB...". */
        ctype = "text/plain";
        static char plog[160];
        preempt_demo(plog, sizeof plog);
        bl = s_put(body, bl, "preempt log: ");
        bl = s_put(body, bl, plog);
        bl = s_put(body, bl, "\n");
    } else if (starts_with(req, "GET /genetirq") || starts_with(req, "POST /genetirq")) {
        /* Diagnostic: is the GENET RX-done interrupt (INTID 189) being
         * delivered?  genet_irq_count advances only in genet_irq_handler.
         * Curl this twice with traffic in between — if it grows, the GIC is
         * routing the GENET SPI and we can build an IRQ-woken net process. */
        ctype = "text/plain";
        bl = s_put(body, bl, "genet_irq=");
        bl = s_putdec(body, bl, (long)genet_irq_count());
        bl = s_put(body, bl, "\n");
    } else if (starts_with(req, "GET /netpreempt") || starts_with(req, "POST /netpreempt")) {
        /* Toggle preemptive networking: with it ON the timer can preempt the
         * app-worker (mid cc/llm) to run the net process, so RX/ICMP/TCP stay
         * low-latency during a long compute.  ?on=1 enable, ?on=0 disable.
         * Safe because NULLPROC is never preempted and the only other ready
         * proc is the net process (which never touches vheap). */
        ctype = "text/plain";
        static char onbuf[8];
        if (q_param(req, "on", onbuf, sizeof onbuf)) {
            g_net_preempt = (onbuf[0] == '1');
            proc_set_preempt(g_net_preempt);
        }
        bl = s_put(body, bl, "net_preempt=");
        bl = s_put(body, bl, g_net_preempt ? "on" : "off");
        bl = s_put(body, bl, "\n");
    } else if (starts_with(req, "GET /netstat") || starts_with(req, "POST /netstat")) {
        /* App-handoff state: IDLE/QUEUED/WORKING/DONE + served count + preempt. */
        ctype = "text/plain";
        static const char *st[] = { "IDLE", "QUEUED", "WORKING", "DONE" };
        int s = g_app_state; if (s < 0 || s > 3) s = 0;
        bl = s_put(body, bl, "app_state=");  bl = s_put(body, bl, st[s]);
        bl = s_put(body, bl, " served=");    bl = s_putdec(body, bl, (long)g_app_served);
        bl = s_put(body, bl, " preempt=");   bl = s_put(body, bl, g_net_preempt ? "on" : "off");
        bl = s_put(body, bl, "\n");
    } else if (starts_with(req, "GET /windows") || starts_with(req, "POST /windows")) {
        /* Read-only window inventory for the Py-I layout designer: id, name,
         * and current geometry of every wm window (JSON). */
        ctype = "application/json";
        int nw = wm_window_count();
        bl = s_put(body, bl, "[");
        for (int i = 0; i < nw; i++) {
            int x = 0, y = 0, ww = 0, hh = 0;
            char nm[40];
            wm_window_get(i, &x, &y, &ww, &hh);
            if (wm_window_name(i, nm, sizeof nm) < 0) nm[0] = 0;
            if (i) bl = s_put(body, bl, ",");
            bl = s_put(body, bl, "{\"id\":");   bl = s_putdec(body, bl, i);
            bl = s_put(body, bl, ",\"name\":\""); bl = s_put(body, bl, nm);
            bl = s_put(body, bl, "\",\"x\":");   bl = s_putdec(body, bl, x);
            bl = s_put(body, bl, ",\"y\":");     bl = s_putdec(body, bl, y);
            bl = s_put(body, bl, ",\"w\":");     bl = s_putdec(body, bl, ww);
            bl = s_put(body, bl, ",\"h\":");     bl = s_putdec(body, bl, hh);
            bl = s_put(body, bl, ",\"fs\":");    bl = s_putdec(body, bl, wm_window_fontscale(i));
            bl = s_put(body, bl, "}");
        }
        bl = s_put(body, bl, "]");
    } else if (starts_with(req, "POST /chat") || starts_with(req, "GET /chat")) {
        /* Converse with a resident actor: deliver the message (POST body or
         * ?m= for GET) as a STRING to actor 0's `say` method and return its
         * reply.  The actor keeps conversation state across calls; if it calls
         * llm(), that reply is LLM-generated. */
        ctype = "text/plain";
        static char msg[256];
        int ml = 0;
        if (req[0] == 'P') {
            int he = find_header_end(req);
            if (he >= 0) { const char *b = req + he;
                while (b[ml] && ml < (int)sizeof(msg)-1) { msg[ml] = b[ml]; ml++; } }
            msg[ml] = 0;
        } else {
            static char enc[256];
            if (q_param(req, "m", enc, sizeof enc)) url_decode(enc, msg, sizeof msg);
        }
        static char creply[700];
        cc_actor_send_str(0, "say", msg, creply, sizeof creply);
        bl = s_put(body, bl, creply);
        bl = s_put(body, bl, "\n");
    } else if (starts_with(req, "POST /actor/load") || starts_with(req, "GET /actor/load")) {
        /* Load an AIPL-generated C program as RESIDENT actors: the body is
         * the C source; main() spawns the actors and they stay alive for
         * later /actor/send messages. */
        ctype = "text/plain";
        static char asrc[7168];
        int aslen = 0;
        if (req[0] == 'P') {
            int he = find_header_end(req);
            if (he >= 0) {
                const char *b = req + he;
                while (b[aslen] && aslen < (int)sizeof(asrc) - 1) { asrc[aslen] = b[aslen]; aslen++; }
                asrc[aslen] = 0;
            }
        } else {
            static char aenc[7168];
            if (q_param(req, "src", aenc, sizeof aenc)) aslen = url_decode(aenc, asrc, sizeof asrc);
        }
        static char ares[1100];
        cc_actor_load(asrc, aslen, ares, sizeof ares);
        bl = s_put(body, bl, ares);
    } else if (path_eq(req, "/actor/send")) {
        /* Exchange a message with a resident actor.  Up to 3 int args:
         *   GET /actor/send?to=<id>&m=<method>&arg=<n>[&a1=<n>&a2=<n>]
         * (arg == a0; a1/a2 let e.g. the Layout actor take move(id,x,y).) */
        ctype = "text/plain";
        int  to  = q_int(req, "to", 0);
        int  a0  = q_int(req, "arg", 0);
        int  a1  = q_int(req, "a1", 0);
        int  a2  = q_int(req, "a2", 0);
        char method[32];
        if (!q_param(req, "m", method, sizeof method)) method[0] = 0;
        static char mres[256];
        cc_actor_send_msg(to, method, a0, a1, a2, mres, sizeof mres);
        bl = s_put(body, bl, mres);
        bl = s_put(body, bl, "\n");
    } else if (path_eq(req, "/send")) {
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
    } else if (path_eq(req, "/")) {
        ctype = "text/plain";
        bl = s_put(body, bl, "xinu-rpi4 (Pi 4) actor HTTP gateway\n"
                             "GET  /api/actors\n"
                             "GET  /send?to=<id>&m=<bump|add|set|get|reset>&arg=<n>\n"
                             "POST /compile      (body = C source; JIT-run, output + => retval)\n"
                             "POST /actor/load   (body = AIPL-generated C; spawns resident actors)\n"
                             "GET  /actor/send?to=<id>&m=<method>&arg=<n>  (message a resident actor)\n");
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

/* True when a complete request is queued for the worker (net process polls
 * this after draining, to wake the worker). */
int tcp_app_req_pending(void) { return g_app_state == APP_QUEUED; }

/* Run the queued request's app processing (http_build -> cc/llm).  Called
 * ONLY from the app-worker process, off the net process's stack, so a long
 * compute doesn't stall RX/ICMP/TCP.  Returns 1 if it produced a response. */
int tcp_app_work(void)
{
    if (g_app_state != APP_QUEUED) return 0;
    g_app_state = APP_WORKING;
    app_beat();                                 /* heartbeat: request started */
    g_app_resp_len = http_build(g_app_req, g_app_resp, (int)sizeof g_app_resp);
    __asm__ volatile ("dsb sy" ::: "memory");   /* resp visible before DONE */
    g_app_state = APP_DONE;
    return 1;
}

/* Send a finished response (PSH+ACK then FIN).  Called ONLY from the net
 * process (sole owner of GENET/TCP).  Drops the response if the connection
 * died (peer RST) while the worker was busy. */
void tcp_app_flush(void)
{
    if (g_app_state != APP_DONE) return;
    if (g_conn.state == TCP_ESTABLISHED) {
        tcp_send(TCP_FLAG_PSH | TCP_FLAG_ACK, g_app_resp, g_app_resp_len);
        g_conn.my_seq += g_app_resp_len;
        tcp_send(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        g_conn.my_seq += 1;
        g_conn.state = TCP_FIN_WAIT_1;
        g_app_served++;
        uart_puts("http: served request (worker), FIN sent\n");
    } else {
        uart_puts("http: connection gone, dropping worker response\n");
    }
    g_app_state = APP_IDLE;
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
            g_httpreqlen = 0;                 /* fresh request accumulator */
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
            /* Accumulate the HTTP request across segments (a POST body —
             * e.g. C source for /compile — may not fit one segment).  We
             * only append in-order data; retransmits are just re-ACKed.
             * Once the full request has arrived we hand it to the worker. */
            if (seq == g_conn.peer_seq) {                 /* in-order: append */
                int space = (int)sizeof(g_httpreq) - 1 - g_httpreqlen;
                int n = data_len < space ? data_len : space;
                for (int i = 0; i < n; i++) g_httpreq[g_httpreqlen++] = (char)data[i];
                g_httpreq[g_httpreqlen] = 0;
                g_conn.peer_seq = seq + data_len;
            }

            if (!request_complete(g_httpreq, g_httpreqlen)) {
                if (g_httpreqlen >= (int)sizeof(g_httpreq) - 1) {
                    /* Buffer full but request still incomplete: refuse
                     * rather than wait forever for data we can't store. */
                    const char *m =
                        "HTTP/1.0 413 Payload Too Large\r\n"
                        "Content-Type: text/plain\r\nConnection: close\r\n\r\n"
                        "cc: request too large for /compile buffer\n";
                    int ml = 0; while (m[ml]) ml++;
                    tcp_send(TCP_FLAG_PSH | TCP_FLAG_ACK, m, ml);
                    g_conn.my_seq += ml;
                    tcp_send(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
                    g_conn.my_seq += 1;
                    g_conn.state = TCP_FIN_WAIT_1;
                    return 1;
                }
                tcp_send(TCP_FLAG_ACK, 0, 0);             /* ack, await the rest */
                return 1;
            }

            /* Complete request: hand it to the app-worker process instead of
             * running http_build (cc/llm) here on the net process's stack.
             * ACK the request now; tcp_app_flush() sends the response when
             * the worker is done.  Only queue when idle (single in-flight). */
            if (g_app_state == APP_IDLE) {
                int n = g_httpreqlen;
                if (n > (int)sizeof(g_app_req) - 1) n = (int)sizeof(g_app_req) - 1;
                for (int i = 0; i < n; i++) g_app_req[i] = g_httpreq[i];
                g_app_req[n] = 0;
                g_app_req_len = n;
                __asm__ volatile ("dsb sy" ::: "memory");  /* req visible before QUEUED */
                g_app_state = APP_QUEUED;
            }
            tcp_send(TCP_FLAG_ACK, 0, 0);   /* ack the request; reply follows */
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
