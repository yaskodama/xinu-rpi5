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
static int g_reqlen;                  /* bytes of HTTP request accumulated so far */
static int g_chain_len;               /* bytes of a kernel image staged for /chainload */
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

/* Build the HTTP response for NUL-terminated request `req` into `out`
 * (capacity `max`).  Returns the byte length. */
static int http_build(const char *req, char *out, int max)
{
    char body[640];
    int  bl = 0;
    const char *ctype = "application/json";
    (void)max;

    char rpath[300];
    http_path(req, rpath, sizeof rpath);          /* request path, for /fs/* */

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
            static char ccout[512];
            long rv = 0;
            int rc = cc_run_source(bodyp, srclen, ccout, (int)sizeof ccout, &rv);
            bl = s_put(body, bl, ccout);
            if (rc == 0) { bl = s_put(body, bl, "=> "); bl = s_putdec(body, bl, rv); bl = s_put(body, bl, "\n"); }
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
    } else if (str_starts(rpath, "/chainload")) {
        /* Network kexec — POST the kernel in chunks to staging RAM, then GO.
         *   POST /chainload?off=<byte>  body = a chunk (<=8 KB)
         *   GET  /chainload?go=1&len=<N> -> relocate trampoline + boot it
         * RAM-only: a bad image just needs a real power-cycle (no SD write). */
        ctype = "text/plain";
        volatile unsigned char *STAGE = (volatile unsigned char *)0x4000000UL;
        if (req[0]=='P') {
            int off = q_int(req, "off", -1);
            const char *b = http_body(req);
            int cl = content_length(req);
            if (off == 0) g_chain_len = 0;
            if (off >= 0 && b && cl > 0) {
                for (int i = 0; i < cl; i++) STAGE[off + i] = (unsigned char)b[i];
                if (off + cl > g_chain_len) g_chain_len = off + cl;
                bl = s_put(body, bl, "ok off="); bl = s_putdec(body, bl, off);
                bl = s_put(body, bl, " n=");     bl = s_putdec(body, bl, cl);
                bl = s_put(body, bl, " total="); bl = s_putdec(body, bl, g_chain_len);
                bl = s_put(body, bl, "\n");
            } else bl = s_put(body, bl, "usage: POST /chainload?off=<byte> body=<chunk>\n");
        } else if (q_int(req, "go", 0)) {
            int len = q_int(req, "len", g_chain_len);
            extern void kernel_chainload(unsigned long, unsigned long);
            kernel_chainload(0x4000000UL, (unsigned long)len);   /* never returns */
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
    } else if (path_eq(req, "/")) {
        ctype = "text/plain";
        bl = s_put(body, bl, "xinu-rpi5 (Pi 5) actor HTTP gateway\n"
                             "GET /api/actors\n"
                             "GET /send?to=<id>&m=<bump|add|set|get|reset>&arg=<n>\n"
                             "POST /cc  (C source in body) -> JIT compile & run\n"
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
            static char g_req[16384];
            static char http_resp[1400];

            /* Only accept in-order data (seq == what we expect); ignore the
             * rest so a retransmit can't corrupt the buffer. */
            if ((unsigned long)seq == g_conn.peer_seq) {
                for (int i = 0; i < data_len && g_reqlen < (int)sizeof(g_req) - 1; i++)
                    g_req[g_reqlen++] = (char)data[i];
                g_req[g_reqlen] = 0;
                g_conn.peer_seq = seq + data_len;
            }
            tcp_send(TCP_FLAG_ACK, 0, 0);          /* acknowledge the segment */

            if (!http_complete(g_req, g_reqlen)) return 1;   /* need more */

            int rlen = http_build(g_req, http_resp, (int)sizeof http_resp);
            g_reqlen = 0;
            tcp_send(TCP_FLAG_PSH | TCP_FLAG_ACK, http_resp, rlen);
            g_conn.my_seq += rlen;

            /* Close immediately (HTTP/1.0 Connection: close). */
            tcp_send(TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
            g_conn.my_seq += 1;
            g_conn.state = TCP_FIN_WAIT_1;
            uart_puts("http: served request, FIN sent\n");
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
