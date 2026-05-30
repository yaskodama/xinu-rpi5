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
extern int llm_run_drain(const char *prompt, int max_new, char *out, int outcap, int echo, int drain);
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

/* Multi-conn (NEXT_SESSION #3, 2026-05-30): the kernel now tracks up to
 * NCONN concurrent TCP connections.  Each connection carries its own
 * 4-tuple, sequence numbers, and partial-HTTP-request accumulator (a
 * single shared g_httpreq would corrupt as soon as two clients sent
 * overlapping segments).  The single g_app_state mailbox still serves
 * ONE request at a time — the other connections can have their TCP
 * accumulation continue in parallel and queue when the worker is free. */
#define NCONN 4

struct tcp_conn {
    int state;
    unsigned char peer_mac[6];
    unsigned char peer_ip[4];
    unsigned short peer_port;
    unsigned long my_seq;       /* next byte we will send */
    unsigned long peer_seq;     /* next byte we expect to receive */
    int greeted;
    /* Per-conn HTTP request accumulator (reassembles a request that
     * spans segments, e.g. /compile POST body).  Reset on each new
     * ESTABLISHED. */
    char httpreq[8192];
    int  httpreqlen;
};

static struct tcp_conn g_conns[NCONN];

static unsigned char g_my_mac[6];
static unsigned char g_my_ip[4]  = { 192, 168, 3, 100 };
static unsigned short g_listen_port = 23;
static unsigned long g_isn_seed = 0xDEADBEEF;
static unsigned long g_dropped_syn;     /* SYNs refused because all slots in use */

/* Find the connection matching a peer 4-tuple, or NULL if none.  Active
 * (non-LISTEN/CLOSED) states only — a free slot doesn't match. */
static struct tcp_conn *find_conn(const volatile unsigned char *peer_ip,
                                  unsigned short peer_port)
{
    for (int i = 0; i < NCONN; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (c->state == TCP_CLOSED || c->state == TCP_LISTEN) continue;
        if (c->peer_port != peer_port) continue;
        int eq = 1;
        for (int k = 0; k < 4; k++) if (c->peer_ip[k] != peer_ip[k]) { eq = 0; break; }
        if (eq) return c;
    }
    return 0;
}

/* Find a free slot to host a new connection (state LISTEN or CLOSED).
 * Returns NULL when all NCONN slots are in use — the SYN gets dropped
 * and the client retries (standard TCP backpressure). */
static struct tcp_conn *alloc_conn(void)
{
    for (int i = 0; i < NCONN; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (c->state == TCP_CLOSED || c->state == TCP_LISTEN) return c;
    }
    return 0;
}

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

/* Per-conn HTTP request buffers live inside struct tcp_conn (above);
 * the old global g_httpreq[] is gone — multiple peers can have partial
 * requests in flight simultaneously without stomping each other. */

/* ---------- app-layer handoff (preemptive networking) ----------
 * The net process must NOT run http_build (cc/llm) inline, or a long
 * compute stalls RX/ICMP/TCP for everyone.  Instead a complete request is
 * handed to a dedicated app-worker process via this single-slot mailbox;
 * the net process keeps draining and later flushes the finished response.
 * GENET/TCP stays owned by the net process; vheap/cc/llm by the worker.
 * One in-flight HTTP REQUEST at a time (the worker is single).  With
 * multi-conn, other connections continue to accumulate their requests
 * in their per-conn httpreq[] buffers — they queue here when the worker
 * is free.  g_app_conn_idx remembers which conn the in-flight request
 * came from so the eventual flush sends to the right peer.
 *
 * State is owned by exactly one side at each step, so a plain volatile int
 * is race-free under preemption: net sets QUEUED (only from IDLE), worker
 * sets WORKING then DONE, net sets IDLE on flush. */
enum { APP_IDLE = 0, APP_QUEUED, APP_WORKING, APP_DONE };
static volatile int g_app_state;
static int          g_app_conn_idx = -1;   /* which g_conns[] slot owns this request */
static char         g_app_req[8192];
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

/* App-worker phase: a short label of what the worker is doing, set at each
 * lock-holding step (here + cc.c).  Points at static string literals so the wm
 * can read it from NULLPROC after a wedge.  Localizes WHERE a stuck app worker
 * (app=WORKING, hb frozen) is — e.g. ph=llm vs ph=ld-main vs ph=disp. */
volatile const char *g_app_phase = "idle";
void        app_phase(const char *p)  { g_app_phase = p; }
const char *rt_phase(void)            { return (const char *)g_app_phase; }

/* --------- AIPL runtime work queue (NEXT_SESSION option ②, phase 1) ---------
 * Separates the cc/llm runtime from the HTTP app worker.  Phase 1: /llm only.
 * The app worker (in http_build's /llm branch) fills g_aipl_work, sets state to
 * SUBMIT, kicks the aipl process, and parks on g_aipl_done_w.  The aipl process
 * (loader/main.c aipl_proc_main) wakes, runs llm_run on its own 32KB stack,
 * sets state to DONE, and kicks the app worker.  Net stays preemptible while
 * aipl works, so a long /llm no longer blocks ICMP (the 176x latency win is
 * naturally preserved by aipl being a preemptible process, not by a separate
 * gate).  Other routes (/compile, /actor/*, /chat) still execute on the app
 * worker for now; they will migrate in subsequent phases. */
enum { AIPL_IDLE = 0, AIPL_SUBMIT, AIPL_RUN, AIPL_DONE };
static volatile int g_aipl_state;
static struct {
    int  kind;                          /* 0 = llm_run                          */
    int  max_new;
    int  echo;
    char prompt[1024];
    char out[700];
    int  outlen;
} g_aipl_work;

/* Wakers/parkers exposed by main.c (waiter_t lives there). */
extern void aipl_kick(void);            /* app -> aipl (work submitted)         */
extern void aipl_done_kick(void);       /* aipl -> app (work finished)          */
extern void aipl_done_wait(void);       /* app: park until aipl kicks back      */
extern void aipl_done_reset(void);      /* app: clear stale pending before submit */
/* fwd: vheap mutex (declared below in this file) */
extern void aipl_lock(void);
extern void aipl_unlock(void);

int  tcp_aipl_pending(void)  { return g_aipl_state == AIPL_SUBMIT; }

void tcp_aipl_work_run(void)
{
    if (g_aipl_state != AIPL_SUBMIT) return;
    g_aipl_state = AIPL_RUN;
    app_phase("aipl-llm");
    if (g_aipl_work.kind == 0) {
        aipl_lock();
        g_aipl_work.outlen = llm_run(
            g_aipl_work.prompt[0] ? g_aipl_work.prompt : (const char *)0,
            g_aipl_work.max_new, g_aipl_work.out, sizeof g_aipl_work.out,
            g_aipl_work.echo);
        aipl_unlock();
    }
    __asm__ volatile ("dsb sy" ::: "memory");
    g_aipl_state = AIPL_DONE;
    app_phase("aipl-done");
    aipl_done_kick();
}

static void tcp_aipl_submit_llm(const char *prompt, int max_new, int echo)
{
    g_aipl_work.kind    = 0;
    g_aipl_work.max_new = max_new;
    g_aipl_work.echo    = echo;
    int i;
    for (i = 0; prompt && prompt[i] && i < (int)sizeof(g_aipl_work.prompt) - 1; i++)
        g_aipl_work.prompt[i] = prompt[i];
    g_aipl_work.prompt[i] = 0;
    g_aipl_work.outlen = 0;
    g_aipl_work.out[0] = 0;
    __asm__ volatile ("dsb sy" ::: "memory");
    g_aipl_state = AIPL_SUBMIT;
}

/* Preemption control (system/proc.c) — toggled at runtime via /netpreempt
 * so preemptive networking can be enabled only after correctness is proven. */
extern void proc_set_preempt(int on);
extern void aipl_lock(void);
extern void aipl_unlock(void);
/* Gate timer preemption while a vheap user runs concurrently with resident
 * actors (system/proc.c, system/actorproc.c).  /llm holds the shared aipl_lock;
 * if it runs preemptible while AIPL actors are resident, a preempt mid-llm
 * races the actor scheduling / GENET IRQ re-arm and wedges (own=2 in llm_run,
 * or ph=idle with the net process never re-woken).  So gate /llm ONLY when
 * actors exist; pure /llm (no actors) keeps full preemption + its latency win. */
extern void proc_actor_pump_enter(void);
extern void proc_actor_pump_leave(void);
extern int  ap_live_count(void);

/* Fault capture (system/exception.c) — surfaced by /fault. */
extern volatile unsigned long g_fault_count;
extern volatile unsigned long g_fault_esr, g_fault_far, g_fault_elr, g_fault_spsr, g_fault_sp;

/* AIPL lock watchdog (system/proc.c) — surfaced by /lockstat. */
extern volatile unsigned long g_lock_timeouts;
extern volatile int           g_lock_stuck_owner, g_lock_stuck_owner_state, g_lock_stuck_by;

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
    /* Mark every slot as LISTEN (free).  alloc_conn() picks them off
     * the array in order as SYNs arrive. */
    for (int i = 0; i < NCONN; i++) {
        g_conns[i].state   = TCP_LISTEN;
        g_conns[i].greeted = 0;
    }
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

/* Build and send a single TCP segment for connection `c`.  `payload` may
 * be NULL (header only).  Returns 0 on success, -1 on failure.  Takes
 * the conn explicitly so the multi-conn rework above doesn't have to
 * stash a "current connection" global. */
static int tcp_send(struct tcp_conn *c,
                    unsigned char flags, const char *payload, int payload_len)
{
    if (payload_len < 0) payload_len = 0;
    int tcp_len = 20 + payload_len;
    int ip_total = 20 + tcp_len;
    int frame_len = 14 + ip_total;
    if (frame_len > 1518) return -1;

    /* zero header span */
    for (int i = 0; i < 54; i++) tx_frame[i] = 0;

    /* --- Ethernet --- */
    for (int i = 0; i < 6; i++) tx_frame[i]     = c->peer_mac[i];
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
    for (int i = 0; i < 4; i++) ih[16 + i] = c->peer_ip[i];
    unsigned short ipsum = ip_checksum((const unsigned char *)ih, 20);
    ih[10] = (unsigned char)(ipsum >> 8);
    ih[11] = (unsigned char)(ipsum & 0xFF);

    /* --- TCP --- */
    volatile unsigned char *th = tx_frame + 34;
    th[0] = (unsigned char)(g_listen_port >> 8);
    th[1] = (unsigned char)(g_listen_port & 0xFF);
    th[2] = (unsigned char)(c->peer_port >> 8);
    th[3] = (unsigned char)(c->peer_port & 0xFF);
    th[4] = (unsigned char)(c->my_seq >> 24);
    th[5] = (unsigned char)(c->my_seq >> 16);
    th[6] = (unsigned char)(c->my_seq >> 8);
    th[7] = (unsigned char)(c->my_seq);
    th[8]  = (unsigned char)(c->peer_seq >> 24);
    th[9]  = (unsigned char)(c->peer_seq >> 16);
    th[10] = (unsigned char)(c->peer_seq >> 8);
    th[11] = (unsigned char)(c->peer_seq);
    th[12] = 0x50;                             /* data offset = 5 (20 bytes) */
    th[13] = flags;
    th[14] = 0x20; th[15] = 0x00;              /* window = 8192 */
    th[16] = 0; th[17] = 0;                    /* checksum (filled below) */
    th[18] = 0; th[19] = 0;                    /* urgent */

    if (payload && payload_len > 0) {
        for (int i = 0; i < payload_len; i++)
            tx_frame[54 + i] = (unsigned char)payload[i];
    }

    unsigned short ucs = tcp_checksum(g_my_ip, c->peer_ip,
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

static int s_puthex(char *b, int pos, unsigned long v)
{
    b[pos++] = '0'; b[pos++] = 'x';
    for (int i = 15; i >= 0; i--) {
        unsigned long nyb = (v >> (i * 4)) & 0xF;
        b[pos++] = (char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10));
    }
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
    int neg = 0, i = 0;
    unsigned int n = 0;
    if (v[0] == '-') { neg = 1; i = 1; }
    /* Hex form 0x...  — needed for ?addr=0x7e0000b4 style probes */
    if (v[i] == '0' && (v[i+1] == 'x' || v[i+1] == 'X')) {
        i += 2;
        for (; v[i]; i++) {
            char c = v[i];
            if (c >= '0' && c <= '9')      n = (n << 4) | (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') n = (n << 4) | (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') n = (n << 4) | (unsigned)(c - 'A' + 10);
            else break;
        }
    } else {
        for (; v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + (unsigned)(v[i] - '0');
    }
    return neg ? -(int)n : (int)n;
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

    if (starts_with(req, "POST /reboot") || starts_with(req, "GET /reboot")) {
        /* Soft-wedge recovery: trigger BCM2711 watchdog reset.  The
         * actor system / cc JIT may be unresponsive, but as long as
         * the network gateway thread is alive we can take this route
         * and avoid a physical power cycle.
         *
         * Note: the SoC reset happens before the HTTP reply gets
         * flushed back, so the client sees `connection closed` / EOF
         * rather than a 200 OK.  That's OK — Mac driver knows what
         * it asked for. */
        extern void pm_reset(void);
        pm_reset();              /* never returns */
        /* unreachable */
    }
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
        app_phase("llm");
        /* The aipl-process route (commit setting up aipl_proc_main as NEXT_SESSION
         * option-② groundwork) regressed this path: /llm-on-aipl stalls in
         * forward() with resident actors present (Runtime ph=llm-fwd, own=3).
         * Reverted to the proven conditional-gate + RX self-drain path — aipl
         * stays a dormant runtime, available for the next phase (e.g. migrate
         * /chat first, or investigate forward()/process interaction). */
        int llm_gated = (ap_live_count() > 0);
        if (llm_gated) proc_actor_pump_enter();
        aipl_lock();
        llm_run_drain(prompt[0] ? prompt : (const char *)0, n, ltxt, sizeof ltxt, 1, llm_gated);
        aipl_unlock();
        if (llm_gated) proc_actor_pump_leave();
        bl = s_put(body, bl, ltxt);
        bl = s_put(body, bl, "\n");
        app_phase("llm-done");
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
    } else if (starts_with(req, "GET /fault") || starts_with(req, "POST /fault")) {
        /* Last CPU fault captured by the exception handler (which now keeps the
         * box alive and spins so this can be read).  count>0 => a fault hit;
         * ESR_EL1 top 6 bits (EC) decode the cause, FAR_EL1 the bad address. */
        ctype = "text/plain";
        bl = s_put(body, bl, "fault_count="); bl = s_putdec(body, bl, (long)g_fault_count);
        bl = s_put(body, bl, " esr=");        bl = s_puthex(body, bl, g_fault_esr);
        bl = s_put(body, bl, " ec=");         bl = s_putdec(body, bl, (long)((g_fault_esr >> 26) & 0x3f));
        bl = s_put(body, bl, " far=");        bl = s_puthex(body, bl, g_fault_far);
        bl = s_put(body, bl, " elr=");        bl = s_puthex(body, bl, g_fault_elr);
        bl = s_put(body, bl, " spsr=");       bl = s_puthex(body, bl, g_fault_spsr);
        bl = s_put(body, bl, " sp=");         bl = s_puthex(body, bl, g_fault_sp);
        bl = s_put(body, bl, "\n");
    } else if (starts_with(req, "GET /lockstat") || starts_with(req, "POST /lockstat")) {
        /* AIPL heap-lock watchdog: timeouts>0 means a holder wedged and the
         * lock was force-stolen.  stuck_owner = the pid that wouldn't release,
         * its state (1=READY 2=CURR 3=WAIT 4=TERM 0=FREE), stuck_by = waiter. */
        ctype = "text/plain";
        bl = s_put(body, bl, "lock_timeouts="); bl = s_putdec(body, bl, (long)g_lock_timeouts);
        bl = s_put(body, bl, " stuck_owner=");  bl = s_putdec(body, bl, (long)g_lock_stuck_owner);
        bl = s_put(body, bl, " owner_state=");  bl = s_putdec(body, bl, (long)g_lock_stuck_owner_state);
        bl = s_put(body, bl, " stuck_by=");     bl = s_putdec(body, bl, (long)g_lock_stuck_by);
        bl = s_put(body, bl, "\n");
    } else if (starts_with(req, "POST /type") || starts_with(req, "GET /type")) {
        /* Network keyboard input — feeds shellwin via xhci_keyboard_event(c)
         * (the hook originally meant for a real xHCI HID driver, which we
         * couldn't bring up; firmware capability-bit gates PCIe on Pi 4 bare-
         * metal — see project memory).  Send a string and each character
         * goes to the active shellwin as if typed.
         *
         *   POST /type             body = literal text
         *   GET  /type?t=Hello%20world          URL-encoded text
         *   GET  /type?key=NAME[,NAME,...]      named special keys
         *   GET  /type?t=ls&key=enter           combine: type "ls" then Enter
         *
         * Named keys (in send order if comma-separated):
         *   enter, tab, esc, bs(=backspace), del, space
         *   up, down, left, right                       (ANSI: ESC [ A/B/D/C)
         *   home, end                                   (ANSI: ESC [ H/F)
         *   ctrl-a .. ctrl-z                            (single byte 0x01-0x1a)
         *   f1..f4                                      (ANSI: ESC O P/Q/R/S)
         * Unknown name → ignored. */
        extern void xhci_keyboard_event(char c);
        ctype = "text/plain";
        static char tbuf[256];
        int tlen = 0;
        if (req[0] == 'P') {
            int he = find_header_end(req);
            if (he >= 0) {
                const char *b = req + he;
                while (b[tlen] && tlen < (int)sizeof(tbuf) - 1) { tbuf[tlen] = b[tlen]; tlen++; }
                tbuf[tlen] = 0;
            }
        } else {
            static char tenc[256];
            if (q_param(req, "t", tenc, sizeof tenc))
                tlen = url_decode(tenc, tbuf, sizeof tbuf);
        }
        for (int i = 0; i < tlen; i++) xhci_keyboard_event(tbuf[i]);
        /* Named special keys (optional, can follow / precede literal text). */
        static char kbuf[128];
        int kbl = 0;
        if (req[0] != 'P' && q_param(req, "key", kbuf, sizeof kbuf)) {
            /* Comma-separate the list — process each name in turn. */
            static const struct { const char *name; const char *seq; } keymap[] = {
                { "enter", "\r" },     { "cr", "\r" },        { "tab", "\t" },
                { "esc", "\x1b" },     { "bs", "\x08" },      { "backspace", "\x08" },
                { "del", "\x7f" },     { "space", " " },
                { "up",    "\x1b[A" }, { "down",  "\x1b[B" },
                { "right", "\x1b[C" }, { "left",  "\x1b[D" },
                { "home",  "\x1b[H" }, { "end",   "\x1b[F" },
                { "f1",    "\x1bOP" }, { "f2",    "\x1bOQ" },
                { "f3",    "\x1bOR" }, { "f4",    "\x1bOS" },
                /* ctrl-a .. ctrl-z handled below by length-check, since
                 * spelling them all out is ~26 lines for little gain. */
            };
            char *p = kbuf;
            while (*p) {
                /* Find end of current name token. */
                char *e = p;
                while (*e && *e != ',') e++;
                int nlen = (int)(e - p);
                if (nlen > 0 && nlen < 16) {
                    /* Match named entry. */
                    int matched = 0;
                    for (unsigned i = 0; i < sizeof(keymap)/sizeof(keymap[0]); i++) {
                        const char *kn = keymap[i].name;
                        int j = 0;
                        while (j < nlen && kn[j] && p[j] == kn[j]) j++;
                        if (j == nlen && kn[j] == 0) {
                            const char *s = keymap[i].seq;
                            while (*s) { xhci_keyboard_event(*s++); kbl++; }
                            matched = 1;
                            break;
                        }
                    }
                    /* ctrl-X pattern: "ctrl-x" → 0x18 etc. */
                    if (!matched && nlen == 6 && p[0]=='c' && p[1]=='t'
                        && p[2]=='r' && p[3]=='l' && p[4]=='-'
                        && p[5]>='a' && p[5]<='z') {
                        xhci_keyboard_event((char)(p[5] - 'a' + 1));
                        kbl++;
                    }
                }
                p = e;
                if (*p == ',') p++;
            }
        }
        bl = s_put(body, bl, "typed ");
        bl = s_putdec(body, bl, (long)(tlen + kbl));
        bl = s_put(body, bl, " char(s)\n");
    } else if (starts_with(req, "GET /click") || starts_with(req, "POST /click")) {
        /* Network mouse input — feeds wm_cursor via xhci_mouse_event(btns,dx,dy).
         * Coordinates are RELATIVE (deltas) to match the xHCI HID convention
         * the underlying function expects.
         *   GET /click?dx=10&dy=-5&btn=1    (btn: bit0=L bit1=R bit2=M)
         * No btn => move only.  Same call works for plain mouse moves. */
        extern void xhci_mouse_event(unsigned nButtons, int dx, int dy);
        int dx  = q_int(req, "dx",  0);
        int dy  = q_int(req, "dy",  0);
        int btn = q_int(req, "btn", 0);
        xhci_mouse_event((unsigned)btn, dx, dy);
        ctype = "text/plain";
        bl = s_put(body, bl, "mouse dx="); bl = s_putdec(body, bl, (long)dx);
        bl = s_put(body, bl, " dy=");      bl = s_putdec(body, bl, (long)dy);
        bl = s_put(body, bl, " btn=");     bl = s_putdec(body, bl, (long)btn);
        bl = s_put(body, bl, "\n");
    } else if (starts_with(req, "GET /pcie") || starts_with(req, "POST /pcie")) {
        /* On-demand probe of the BCM2711 PCIe-1 controller registers.  Fault-safe:
         * recover_spin in exception.c keeps the box alive if MMIO faults. */
        extern int xhci_pcie_dump_html(char *out, int max);
        ctype = "text/plain";
        bl += xhci_pcie_dump_html(body + bl, (int)sizeof body - bl);
    } else if (starts_with(req, "GET /cprman-read") || starts_with(req, "POST /cprman-read")) {
        /* Direct MMIO dump of CPRMAN register block around 0x120-0x140 plus
         * known-active EMMC2 CTL for comparison.  CPRMAN is always-on so reads
         * are safe.  BUSY bit (bit 7) set = clock is actually running. */
        extern unsigned int xhci_cprman_read(unsigned int offset);
        ctype = "text/plain";
        static const struct { const char *name; unsigned int off; } addrs[] = {
            { "EMMC2CTL [+0x1d0] (known active, BUSY should=1) ", 0x1D0 },
            { "EMMC2DIV [+0x1d4]                               ", 0x1D4 },
            { "PCIE?CTL [+0x128] (just wrote, BUSY=?)          ", 0x128 },
            { "PCIE?DIV [+0x12c]                               ", 0x12C },
            { "         +0x120                                 ", 0x120 },
            { "         +0x124                                 ", 0x124 },
            { "         +0x130                                 ", 0x130 },
            { "         +0x134                                 ", 0x134 },
            { "         +0x138                                 ", 0x138 },
            { "         +0x13c                                 ", 0x13C },
            { "         +0x140                                 ", 0x140 },
            /* +0x180..+0x1C0: caused body-buffer overflow (1400 cap) — pruned.
             * Linux clk-bcm2835.c: +0x1B0=CM_ARMCTL, +0x1C0=CM_EMMCCTL. */
            { "PLLA_CTRL[+0x104]                               ", 0x104 },
            { "PLLC_CTRL[+0x108]                               ", 0x108 },
            { "PLLD_CTRL[+0x10c]                               ", 0x10C },
            { "PLLH_CTRL[+0x110]                               ", 0x110 },
        };
        for (unsigned i = 0; i < sizeof(addrs)/sizeof(addrs[0]); i++) {
            bl = s_put(body, bl, addrs[i].name);
            bl = s_put(body, bl, " = ");
            bl = s_puthex(body, bl, xhci_cprman_read(addrs[i].off));
            bl = s_put(body, bl, "\n");
        }
    } else if (starts_with(req, "GET /cprman-axi") || starts_with(req, "POST /cprman-axi")) {
        /* DISABLED: 0x7E1011B0 = CM_ARMCTL (ARM core clock CTL) per
         * Linux clk-bcm2835.c.  Writing 0x5A001000 would change the CPU
         * clock and brick the box.  Route kept as a marker so we never
         * re-introduce the same bug.  Sub-agent that found this register
         * misidentified it as PCIe — it's actually ARM core CTL. */
        ctype = "text/plain";
        bl = s_put(body, bl, "DISABLED: 0x1B0 = CM_ARMCTL, would brick the box\n");
    } else if (starts_with(req, "GET /cprman-init-osc") || starts_with(req, "POST /cprman-init-osc")) {
        /* Same as /cprman-init but with SRC=1 (OSC 19.2MHz, always alive).
         * Definitive test: if BUSY=1 here, 0x128 is a generic CPRMAN clock
         * register and the SRC=6 source PLL must be enabled separately. */
        extern int xhci_cprman_enable_pcie_src(unsigned int src);
        int post_ctl = xhci_cprman_enable_pcie_src(1);
        ctype = "text/plain";
        bl = s_put(body, bl, "cprman-init-osc: wrote 0x5A000011 -> CPRMAN+0x128 (SRC=OSC)\n");
        bl = s_put(body, bl, "post-write CTL = ");
        bl = s_puthex(body, bl, (unsigned int)post_ctl);
        bl = s_put(body, bl, "\nBUSY (bit 7) set => 0x128 is a real clock reg; SRC=6 PLL was the problem\n");
    } else if (starts_with(req, "GET /pcie-clk-full") || starts_with(req, "POST /pcie-clk-full")) {
        /* Replay the FULL firmware PCIe-bring-up sequence (multi-block:
         * 0xFEE01000 + CPRMAN + 0xFEC11010/14).  See xhci.c for full
         * disasm reference.  Reports final CPRMAN+0x128 value — if
         * BUSY (bit 7) finally sets, clock is alive. */
        extern int xhci_pcie_clk_full_sequence(void);
        int ctl = xhci_pcie_clk_full_sequence();
        ctype = "text/plain";
        bl = s_put(body, bl, "pcie-clk-full: ran 11-step firmware sequence\n");
        bl = s_put(body, bl, "CPRMAN+0x128 = ");
        bl = s_puthex(body, bl, (unsigned int)ctl);
        bl = s_put(body, bl, "\n(BUSY=bit 7 should be set if clock actually running)\n");
    } else if (starts_with(req, "GET /cprman-init") || starts_with(req, "POST /cprman-init")) {
        /* DIRECT CPRMAN MMIO write: replay start4.elf disasm pattern at
         * vaddr 0xed4995e — ungate suspected PCIe clock at CPRMAN+0x128.
         * CPRMAN is always-on so MMIO won't hang the bus.  Risk: if 0x128
         * isn't PCIe we kill some other clock and box dies. */
        extern int xhci_cprman_enable_pcie(void);
        int post_ctl = xhci_cprman_enable_pcie();
        ctype = "text/plain";
        bl = s_put(body, bl, "cprman-init: wrote 0x5A000016 -> CPRMAN+0x128\n");
        bl = s_put(body, bl, "post-write CTL = ");
        bl = s_puthex(body, bl, (unsigned int)post_ctl);
        bl = s_put(body, bl, "\n(expect ENAB|SRC=6=0x16 and BUSY bit 7 set if clock is alive)\n");
    } else if (starts_with(req, "GET /cprman") || starts_with(req, "POST /cprman")) {
        /* READ key CPRMAN registers via firmware mailbox proxy.  tag-resp:
         * 0x80000004 = firmware handled (value valid), 0 = ignored (value bogus). */
        extern int xhci_periph_read(unsigned int addr, unsigned int *out, unsigned int *resp);
        extern int xhci_firmware_revision(unsigned int *out, unsigned int *resp);
        ctype = "text/plain";
        unsigned int v, rs;
        /* Sanity: firmware revision (known-good tag) — proves mailbox plumbing OK. */
        bl = s_put(body, bl, "fw_revision: ");
        if (xhci_firmware_revision(&v, &rs) == 0) {
            bl = s_puthex(body, bl, v);
            bl = s_put(body, bl, " (tag-resp=");
            bl = s_puthex(body, bl, rs);
            bl = s_put(body, bl, ")\n");
        } else {
            bl = s_put(body, bl, "MBOX_FAIL\n");
        }
        /* Try three address formats — ARM 0xFE.. / VC 0x7E.. / bus 0x4_7E.. —
         * to find which one firmware accepts for GET_PERIPH_REG. */
        static const struct { const char *name; unsigned int addr; } regs[] = {
            /* ARM physical (32-bit) */
            { "ARM EMMC2CTL [0xfe1011d0]", 0xFE1011D0 },
            { "ARM PCIE?CTL [0xfe101128]", 0xFE101128 },
            /* VC view (BCM legacy) */
            { "VC  EMMC2CTL [0x7e1011d0]", 0x7E1011D0 },
            { "VC  PCIE?CTL [0x7e101128]", 0x7E101128 },
            /* low-32 of bus addr (high bits dropped, in case it's that) */
            { "BUS EMMC2CTL [0xfe1011d0]", 0xFE1011D0 }, /* same as ARM */
            /* small offset variations */
            { "raw 0x7e1011d4         ", 0x7E1011D4 },
            { "raw 0xfe1011d4         ", 0xFE1011D4 },
        };
        for (unsigned i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
            bl = s_put(body, bl, regs[i].name);
            bl = s_put(body, bl, " = ");
            if (xhci_periph_read(regs[i].addr, &v, &rs) == 0) {
                bl = s_puthex(body, bl, v);
                bl = s_put(body, bl, " (tag-resp=");
                bl = s_puthex(body, bl, rs);
                bl = s_put(body, bl, ")");
            } else {
                bl = s_put(body, bl, "MBOX_FAIL");
            }
            bl = s_put(body, bl, "\n");
        }
    } else if (starts_with(req, "GET /mmio-read") || starts_with(req, "POST /mmio-read")) {
        /* Direct MMIO read via safe_mmio_read32 (setjmp around the load —
         * sync faults return -1 instead of wedging the worker).  Use
         * ?addr=0xFE0000B4 to probe the disassembly-identified PCIe gate.
         * NOTE: bus-hang faults (no slave response) bypass setjmp so this
         * can still lock the box; only use for addresses you have some
         * reason to believe are mapped.  Default addr 0xFE0000B4 (gate). */
        extern int safe_mmio_read32(unsigned long addr, unsigned int *out);
        ctype = "text/plain";
        unsigned long addr = (unsigned long)(unsigned int)q_int(req, "addr", 0xFE0000B4);
        unsigned int v = 0;
        int rc = safe_mmio_read32(addr, &v);
        bl = s_put(body, bl, "addr=");
        bl = s_puthex(body, bl, (unsigned int)addr);
        if (rc == 0) {
            bl = s_put(body, bl, " val=");
            bl = s_puthex(body, bl, v);
            bl = s_put(body, bl, " bit0=");
            bl = s_put(body, bl, (v & 1U) ? "1" : "0");
        } else {
            bl = s_put(body, bl, " FAULT (unmapped or rejected)");
        }
        bl = s_put(body, bl, "\n");
    } else if (starts_with(req, "POST /mmio-write")) {
        /* Direct MMIO write via safe_mmio_write32.  POST-only to avoid
         * accidental GETs.  Use ?addr=0xFE0000B4&val=0x1 to force the PCIe
         * gate.  Reports rc + read-back so we can tell if the write stuck
         * (real register vs read-only memory vs fault). */
        extern int safe_mmio_write32(unsigned long addr, unsigned int val);
        extern int safe_mmio_read32(unsigned long addr, unsigned int *out);
        ctype = "text/plain";
        unsigned long addr = (unsigned long)(unsigned int)q_int(req, "addr", 0xFE0000B4);
        unsigned int val  = (unsigned int)q_int(req, "val", 1);
        unsigned int before = 0, after = 0;
        safe_mmio_read32(addr, &before);
        int wrc = safe_mmio_write32(addr, val);
        safe_mmio_read32(addr, &after);
        bl = s_put(body, bl, "addr="); bl = s_puthex(body, bl, (unsigned int)addr);
        bl = s_put(body, bl, " before="); bl = s_puthex(body, bl, before);
        bl = s_put(body, bl, " wrote="); bl = s_puthex(body, bl, val);
        bl = s_put(body, bl, " rc="); bl = s_putdec(body, bl, (long)wrc);
        bl = s_put(body, bl, " after="); bl = s_puthex(body, bl, after);
        bl = s_put(body, bl, (after == val) ? " [STUCK]\n" : " [NOT-STUCK]\n");
    } else if (starts_with(req, "GET /mmio-sweep") || starts_with(req, "POST /mmio-sweep")) {
        /* Sweep a small range of addresses around base ?addr=... step ?step=4
         * count ?n=8.  Useful for seeing if the PCIe gate is part of a wider
         * register block we can recognize.  Default sweeps 0xFE000080..0xFE0000B4
         * which contains the gate (0xFE0000B4 = base+0x34 from firmware r3). */
        extern int safe_mmio_read32(unsigned long addr, unsigned int *out);
        ctype = "text/plain";
        unsigned long base = (unsigned long)(unsigned int)q_int(req, "addr", 0xFE000080);
        int step = q_int(req, "step", 4);
        int n    = q_int(req, "n",    16);
        if (n > 32) n = 32;
        for (int i = 0; i < n; i++) {
            unsigned long a = base + (unsigned long)(i * step);
            unsigned int v = 0;
            int rc = safe_mmio_read32(a, &v);
            bl = s_puthex(body, bl, (unsigned int)a);
            bl = s_put(body, bl, " = ");
            if (rc == 0) bl = s_puthex(body, bl, v);
            else         bl = s_put(body, bl, "FAULT");
            bl = s_put(body, bl, "\n");
        }
    } else if (starts_with(req, "GET /pcie-fw-probe1") || starts_with(req, "POST /pcie-fw-probe1")) {
        /* Single-address mailbox probe — query a SPECIFIC peripheral address
         * to characterize firmware proxy behavior without risk of wedging on
         * a different address.  Use ?addr=0x7e0000b4 for the gate; defaults
         * to the gate if no addr given.  UART trace marks BEFORE/AFTER so
         * the serial log identifies hang-causing addresses precisely. */
        extern int xhci_pcie_fw_probe_one(char *out, int max, unsigned int addr);
        ctype = "text/plain";
        unsigned int addr = (unsigned int)q_int(req, "addr", 0x7E0000B4);
        bl += xhci_pcie_fw_probe_one(body + bl, (int)sizeof body - bl, addr);
    } else if (starts_with(req, "GET /pcie-fw-probe") || starts_with(req, "POST /pcie-fw-probe")) {
        /* Multi-address sweep (safe subset only — skips known-risky
         * addresses).  For unknown addresses use /pcie-fw-probe1?addr=...
         * one at a time so a wedge only kills one HTTP request. */
        extern int xhci_pcie_fw_probe(char *out, int max);
        ctype = "text/plain";
        bl += xhci_pcie_fw_probe(body + bl, (int)sizeof body - bl);
    } else if (starts_with(req, "POST /pcie-fw-gate-force")) {
        /* Try to set 0x7E0000B4 |= 1 via mailbox SET_PERIPH_REG (the firmware
         * gate at ec63fec).  Reports before/after gate value + CPRMAN PCIe
         * CTL after, so we can see whether the write propagated.  POST-only
         * to avoid accidental GET. */
        extern int xhci_pcie_fw_gate_force(char *out, int max);
        ctype = "text/plain";
        bl += xhci_pcie_fw_gate_force(body + bl, (int)sizeof body - bl);
    } else if (starts_with(req, "GET /pcie-init") || starts_with(req, "POST /pcie-init")) {
        /* On-demand BCM2711 PCIe RC bring-up (Linux pcie-brcmstb.c sequence).
         * After this, /pcie should show non-zero registers and PCIE_STATUS
         * should have DL_ACTIVE set. */
        extern int xhci_pcie_bring_up(void);
        int rc = xhci_pcie_bring_up();
        ctype = "text/plain";
        bl = s_put(body, bl, "pcie-init rc=");
        bl = s_putdec(body, bl, (long)rc);
        bl = s_put(body, bl, " (0=link-up, -1=mmio fault, -2=link timeout)\n");
    } else if (starts_with(req, "GET /xhci-reset") || starts_with(req, "POST /xhci-reset")) {
        /* On-demand VC mailbox notify-xhci-reset call.  Isolated so a hung
         * mailbox can't wedge the boot (the original failure mode). */
        extern int xhci_notify_reset_call(void);
        int rc = xhci_notify_reset_call();
        ctype = "text/plain";
        bl = s_put(body, bl, "notify-xhci-reset rc=");
        bl = s_putdec(body, bl, (long)rc);
        bl = s_put(body, bl, "\n");
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
    app_phase("req");
    g_app_resp_len = http_build(g_app_req, g_app_resp, (int)sizeof g_app_resp);
    app_phase("done");
    __asm__ volatile ("dsb sy" ::: "memory");   /* resp visible before DONE */
    g_app_state = APP_DONE;
    return 1;
}

/* Send a finished response (PSH+ACK then FIN).  Called ONLY from the net
 * process (sole owner of GENET/TCP).  Drops the response if the connection
 * died (peer RST) while the worker was busy. */
/* Hand a queued conn's complete HTTP request to the (now idle) app worker.
 * Caller must already have established that g_app_state == APP_IDLE.
 * Returns the conn slot it queued, or -1 if none ready. */
static int queue_to_app(struct tcp_conn *c)
{
    int n = c->httpreqlen;
    if (n > (int)sizeof(g_app_req) - 1) n = (int)sizeof(g_app_req) - 1;
    for (int i = 0; i < n; i++) g_app_req[i] = c->httpreq[i];
    g_app_req[n] = 0;
    g_app_req_len  = n;
    g_app_conn_idx = (int)(c - g_conns);
    __asm__ volatile ("dsb sy" ::: "memory");
    g_app_state    = APP_QUEUED;
    return g_app_conn_idx;
}

/* After flushing a response we may have OTHER connections sitting on
 * fully-arrived requests that the busy worker couldn't accept earlier.
 * Walk g_conns and queue one of them; the net loop will kick the
 * worker next iteration.  Pick FCFS-ish by slot index — good enough
 * given NCONN is tiny. */
static void drain_pending_to_app(void)
{
    if (g_app_state != APP_IDLE) return;
    for (int i = 0; i < NCONN; i++) {
        struct tcp_conn *c = &g_conns[i];
        if (c->state != TCP_ESTABLISHED) continue;
        if (c->httpreqlen <= 0) continue;
        if (!request_complete(c->httpreq, c->httpreqlen)) continue;
        queue_to_app(c);
        return;
    }
}

void tcp_app_flush(void)
{
    if (g_app_state != APP_DONE) return;
    struct tcp_conn *c = (g_app_conn_idx >= 0 && g_app_conn_idx < NCONN)
                       ? &g_conns[g_app_conn_idx] : 0;
    if (c && c->state == TCP_ESTABLISHED) {
        tcp_send(c, TCP_FLAG_PSH | TCP_FLAG_ACK, g_app_resp, g_app_resp_len);
        c->my_seq += g_app_resp_len;
        tcp_send(c, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        c->my_seq += 1;
        c->state = TCP_FIN_WAIT_1;
        g_app_served++;
        uart_puts("http: served request (worker), FIN sent\n");
    } else {
        uart_puts("http: connection gone, dropping worker response\n");
    }
    g_app_conn_idx = -1;
    g_app_state = APP_IDLE;
    app_phase("idle");
    /* Multi-conn drain: another conn may have a complete request that
     * the busy worker couldn't accept earlier — pick it up now so the
     * client doesn't time out. */
    drain_pending_to_app();
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

    /* Try matching against an existing connection 4-tuple first.  A SYN
     * that matches an active conn is a retransmit (or a stray); the
     * fall-through to the SYN-handling branch below covers the truly
     * new-connection case. */
    struct tcp_conn *c = find_conn(ip + 12, sport);

    if (c == 0) {
        /* No existing connection.  Only a SYN can open one. */
        if (!(flags & TCP_FLAG_SYN)) return 1;       /* drop stray */

        c = alloc_conn();
        if (c == 0) {
            /* All NCONN slots in use — drop the SYN.  TCP client will
             * retry after a few hundred ms; standard backpressure. */
            g_dropped_syn++;
            return 1;
        }
        /* Capture peer */
        for (int i = 0; i < 6; i++) c->peer_mac[i] = frame[6 + i];
        for (int i = 0; i < 4; i++) c->peer_ip[i]  = ip[12 + i];
        c->peer_port = sport;
        c->peer_seq  = seq + 1;                  /* ack the SYN */
        c->my_seq    = g_isn_seed;
        g_isn_seed  += 0x100;
        c->greeted   = 0;
        c->httpreqlen = 0;

        uart_puts("tcp: SYN from "); puts_ip(c->peer_ip);
        uart_puts(":"); puts_u32_dec(sport); uart_puts(" -> SYN+ACK\n");

        /* Send SYN+ACK.  SYN occupies one sequence number, so the
         * next byte we send (the greeting) starts at my_seq + 1. */
        g_syn_seen++;
        if (tcp_send(c, TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0) < 0) g_txfail++;
        else                                                    g_synack_sent++;
        c->my_seq += 1;
        c->state   = TCP_SYN_RCVD;
        return 1;
    }

    if (c->state == TCP_SYN_RCVD) {
        if (flags & TCP_FLAG_RST) { c->state = TCP_LISTEN; return 1; }
        if ((flags & TCP_FLAG_ACK) && ack == c->my_seq) {
            c->state = TCP_ESTABLISHED;
            g_estab++;
            c->httpreqlen = 0;                 /* fresh request accumulator */
            uart_puts("tcp: ESTABLISHED (await HTTP request)\n");
            /* HTTP: the client sends the request first; we reply when
             * its data arrives (see the ESTABLISHED data path). */
            return 1;
        }
        return 1;
    }

    if (c->state == TCP_ESTABLISHED) {
        if (flags & TCP_FLAG_RST) {
            uart_puts("tcp: RST -> CLOSED\n");
            c->state = TCP_LISTEN;
            return 1;
        }
        if (data_len > 0) {
            /* Accumulate the HTTP request across segments (a POST body —
             * e.g. C source for /compile — may not fit one segment).  We
             * only append in-order data; retransmits are just re-ACKed.
             * Once the full request has arrived we hand it to the worker. */
            if (seq == c->peer_seq) {                 /* in-order: append */
                int space = (int)sizeof(c->httpreq) - 1 - c->httpreqlen;
                int n = data_len < space ? data_len : space;
                for (int i = 0; i < n; i++) c->httpreq[c->httpreqlen++] = (char)data[i];
                c->httpreq[c->httpreqlen] = 0;
                c->peer_seq = seq + data_len;
            }

            if (!request_complete(c->httpreq, c->httpreqlen)) {
                if (c->httpreqlen >= (int)sizeof(c->httpreq) - 1) {
                    /* Buffer full but request still incomplete: refuse
                     * rather than wait forever for data we can't store. */
                    const char *m =
                        "HTTP/1.0 413 Payload Too Large\r\n"
                        "Content-Type: text/plain\r\nConnection: close\r\n\r\n"
                        "cc: request too large for /compile buffer\n";
                    int ml = 0; while (m[ml]) ml++;
                    tcp_send(c, TCP_FLAG_PSH | TCP_FLAG_ACK, m, ml);
                    c->my_seq += ml;
                    tcp_send(c, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
                    c->my_seq += 1;
                    c->state = TCP_FIN_WAIT_1;
                    return 1;
                }
                tcp_send(c, TCP_FLAG_ACK, 0, 0);             /* ack, await the rest */
                return 1;
            }

            /* Complete request: hand it to the app-worker process instead of
             * running http_build (cc/llm) here on the net process's stack.
             * ACK the request now; tcp_app_flush() sends the response when
             * the worker is done.  Only queue when idle (single in-flight).
             * If busy, the request sits in c->httpreq until tcp_app_flush()
             * picks it up via drain_pending_to_app() — the client just
             * waits longer instead of getting an empty response. */
            if (g_app_state == APP_IDLE) queue_to_app(c);
            tcp_send(c, TCP_FLAG_ACK, 0, 0);   /* ack the request; reply follows */
            return 1;
        }
        if (flags & TCP_FLAG_FIN) {
            c->peer_seq = seq + 1;
            tcp_send(c, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
            c->my_seq += 1;
            c->state = TCP_LAST_ACK;
            uart_puts("tcp: peer FIN, sent FIN+ACK\n");
            return 1;
        }
        return 1;
    }

    if (c->state == TCP_FIN_WAIT_1) {
        if ((flags & TCP_FLAG_ACK) && ack == c->my_seq) {
            if (flags & TCP_FLAG_FIN) {
                c->peer_seq = seq + 1;
                tcp_send(c, TCP_FLAG_ACK, 0, 0);
                c->state = TCP_LISTEN;
                uart_puts("tcp: closed (back to LISTEN)\n");
            }
            return 1;
        }
        return 1;
    }

    if (c->state == TCP_LAST_ACK) {
        if ((flags & TCP_FLAG_ACK) && ack == c->my_seq) {
            c->state = TCP_LISTEN;
            uart_puts("tcp: LAST_ACK -> LISTEN\n");
        }
        return 1;
    }

    return 1;
}

/* Helper: state name (used by both the per-slot dump and the live HDMI
 * panel).  Kept in one place so changes propagate. */
static const char *tcp_state_name(int s)
{
    switch (s) {
        case TCP_CLOSED:      return "CLOSED";
        case TCP_LISTEN:      return "LISTEN";
        case TCP_SYN_RCVD:    return "SYN_RCVD";
        case TCP_ESTABLISHED: return "ESTAB";
        case TCP_FIN_WAIT_1:  return "FINWAIT1";
        case TCP_LAST_ACK:    return "LASTACK";
        default:              return "?";
    }
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
    uart_puts("tcp: listen_port="); puts_u32_dec(g_listen_port);
    uart_puts(" any="); puts_u32_dec(g_tcp_any);
    uart_puts(" seg_rx=");  puts_u32_dec(g_seg_rx);
    uart_puts(" syn=");     puts_u32_dec(g_syn_seen);
    uart_puts(" synack=");  puts_u32_dec(g_synack_sent);
    uart_puts(" estab=");   puts_u32_dec(g_estab);
    uart_puts(" txfail=");  puts_u32_dec(g_txfail);
    uart_puts(" drop_syn=");puts_u32_dec(g_dropped_syn);
    uart_puts("\n");
    for (int i = 0; i < NCONN; i++) {
        struct tcp_conn *c = &g_conns[i];
        uart_puts("  conn[");  puts_u32_dec(i);  uart_puts("] state=");
        uart_puts(tcp_state_name(c->state));
        if (c->state != TCP_LISTEN && c->state != TCP_CLOSED) {
            uart_puts(" peer="); puts_ip(c->peer_ip);
            uart_puts(":");     puts_u32_dec(c->peer_port);
        }
        uart_puts("\n");
    }
    uart_puts("  last_peer=");
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

/* HDMI panel display: report the "most active" connection's state, with
 * a count of currently-active slots appended so e.g. ESTAB(3/4) means
 * three of the four conn slots are past LISTEN. */
const char *tcp_state_str(void)
{
    /* Pick the highest-priority active state for display, in this order:
     *   ESTABLISHED > SYN_RCVD > FIN_WAIT_1 > LAST_ACK > LISTEN
     * (LISTEN if nothing's active). */
    int best = TCP_LISTEN;
    static const int prio[] = {
        [TCP_CLOSED] = 0, [TCP_LISTEN] = 1, [TCP_LAST_ACK] = 2,
        [TCP_FIN_WAIT_1] = 3, [TCP_SYN_RCVD] = 4, [TCP_ESTABLISHED] = 5,
    };
    for (int i = 0; i < NCONN; i++) {
        int s = g_conns[i].state;
        if (prio[s] > prio[best]) best = s;
    }
    return tcp_state_name(best);
}
