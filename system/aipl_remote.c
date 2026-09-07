/* system/aipl_remote.c — AIPL の remote("host:port","actor") をこの板で実現する。
 *
 * 正典 AIPL では、送信先は「アクター参照が入った変数」か
 * remote("host:port", "actorName") である。後者は他ノードのアクターを指す。
 * この板の TCP は受け側しか無い（system/tcp_server.c は passive open だけ）ので、
 * 要求／応答は UDP で運ぶ。
 *
 * 電文は人が読める ASCII 一行。tcpdump や nc でそのまま覗けるようにしてある。
 *
 *   要求  Q <reqid> <actor> <method> <arg...>\n
 *   応答  R <reqid> <値>\n
 *
 * <arg...> は行末までなので、空白を含む文字列も渡せる。
 * <値> は相手の板が v_render で描いたもの（"42" / "true" / 文字列）。
 *
 * ★ 宛先 MAC の決め方。ARP を自分で引くと、要求・応答・再送の状態機械が
 *   もう一つ増える。代わりに
 *     - 最初の要求は「宛先 MAC = ブロードキャスト、宛先 IP = ユニキャスト」で出す。
 *       スイッチは撒くが、IP が一致した板だけが拾う（拾う判断はこのファイルの中）。
 *     - 応答は要求フレームの送り主 MAC へユニキャストで返す。ARP は要らない。
 *     - 応答が届いたら、その MAC を相手 IP に紐づけて覚える。二回目からは
 *       ユニキャストで出る。
 *   実験室の 3 台には十分で、ARP の再実装より壊れにくい。
 */

#include <stddef.h>

extern int  genet_tx_frame(const unsigned char *frame, int length);
extern void net_responder_get_ip(unsigned char out[4]);
extern void net_responder_get_mac(unsigned char out[6]);
extern unsigned long timer_ticks(void);

/* cc.c 側。公開アクターの表と、名前で呼ぶ受け口。 */
extern int         cc_web_count(void);
extern const char *cc_web_path_at(int i);
extern int         cc_web_actor_at(int i);
extern int         cc_remote_dispatch(int actor, const char *method,
                                      const char *arg, char *out, int outcap);

#define AIPL_PORT 9010

/* ---- 覚えた相手の MAC（小さな表で足りる） --------------------------------- */
#define PEER_MAX 8
static struct { unsigned char ip[4], mac[6]; unsigned char used; } g_peer[PEER_MAX];

static void peer_learn(const unsigned char *ip, const unsigned char *mac)
{
    int i, free_slot = -1;
    for (i = 0; i < PEER_MAX; i++) {
        if (!g_peer[i].used) { if (free_slot < 0) free_slot = i; continue; }
        if (g_peer[i].ip[0]==ip[0] && g_peer[i].ip[1]==ip[1]
         && g_peer[i].ip[2]==ip[2] && g_peer[i].ip[3]==ip[3]) {
            for (int k = 0; k < 6; k++) g_peer[i].mac[k] = mac[k];
            return;
        }
    }
    if (free_slot < 0) free_slot = 0;          /* 溢れたら先頭を上書き */
    for (i = 0; i < 4; i++) g_peer[free_slot].ip[i]  = ip[i];
    for (i = 0; i < 6; i++) g_peer[free_slot].mac[i] = mac[i];
    g_peer[free_slot].used = 1;
}
static int peer_lookup(const unsigned char *ip, unsigned char *mac)
{
    for (int i = 0; i < PEER_MAX; i++)
        if (g_peer[i].used
         && g_peer[i].ip[0]==ip[0] && g_peer[i].ip[1]==ip[1]
         && g_peer[i].ip[2]==ip[2] && g_peer[i].ip[3]==ip[3]) {
            for (int k = 0; k < 6; k++) mac[k] = g_peer[i].mac[k];
            return 1;
        }
    return 0;
}

/* ---- 計器 ----------------------------------------------------------------
   remote が黙って失敗したとき、どこで落ちているのかを外から読むための数。
   「板が生きている」ことと「電文が通っている」ことは別なので、
   推測でコードを直す前にここを見る。GET /version が出す。 */
static unsigned long g_n_tx_q, g_n_rx_q, g_n_rx_r, g_n_rx_match, g_n_timeout, g_n_retx;
/* 突き合わせが外れたときに、何と何を比べたのかを残す。
   数だけでは「番号が違う」のか「待っていなかった」のかが分かれない。 */
static long g_dbg_seen_id = -1, g_dbg_wait_at_seen = -2;
/* 送った時刻と、応答を見た時刻（100Hz の刻み）。差が期限に張り付いていれば
   「実行が終わってから届いた」、ほぼ 0 なら「届いているのに噛み合わない」。 */
static unsigned long g_dbg_send_t, g_dbg_seen_t, g_dbg_loops;
void aipl_remote_stats(unsigned long *o)
{ o[0]=g_n_tx_q; o[1]=g_n_rx_q; o[2]=g_n_rx_r; o[3]=g_n_rx_match; o[4]=g_n_timeout;
  o[5]=(unsigned long)g_dbg_seen_id; o[6]=(unsigned long)g_dbg_wait_at_seen;
  o[7]=g_dbg_send_t; o[8]=g_dbg_seen_t; o[9]=g_dbg_loops; o[10]=g_n_retx; }

/* ---- 応答の控え（at-most-once） ------------------------------------------
   要求は再送する（UDP なので落ちる）。素朴に作ると相手のメソッドが二度走る。
   (送り主 IP, reqid) で最後の答えを覚えておき、同じものが来たら実行せずに
   控えを返す。これで「高々一度」になる。 */
#define ANS_MAX 4
static struct { unsigned char ip[4]; int id; char val[160]; unsigned char used; } g_ans[ANS_MAX];
static int g_ans_next;

static const char *ans_lookup(const unsigned char *ip, int id)
{
    for (int i = 0; i < ANS_MAX; i++)
        if (g_ans[i].used && g_ans[i].id == id
         && g_ans[i].ip[0]==ip[0] && g_ans[i].ip[1]==ip[1]
         && g_ans[i].ip[2]==ip[2] && g_ans[i].ip[3]==ip[3])
            return g_ans[i].val;
    return 0;
}
static void ans_store(const unsigned char *ip, int id, const char *val)
{
    int i = g_ans_next; g_ans_next = (g_ans_next + 1) % ANS_MAX;
    for (int k = 0; k < 4; k++) g_ans[i].ip[k] = ip[k];
    g_ans[i].id = id;
    { int k = 0; while (val[k] && k < (int)sizeof g_ans[0].val - 1) { g_ans[i].val[k] = val[k]; k++; }
      g_ans[i].val[k] = 0; }
    g_ans[i].used = 1;
}

/* ---- 待っている応答（同時に 1 本） --------------------------------------- */
/* ---- メッシュの収集 -------------------------------------------------------
   remote(...) は答えを一つだけ待つので、待ち口も一つで足りた。メッシュは
   「返ってきた分だけ」を集めるので、送り主ごとの控えが要る。gather / neighbors
   が待っている間だけ g_mwait_id が立ち、受信側がここへ積む。 */
#define MESH_MAX 8
static volatile int  g_mwait_id = -1;      /* 収集中の reqid。-1 = 収集していない */
static volatile char g_mwait_kind = 0;     /* 'R' = gather / 'A' = neighbors */
static volatile int  g_mn;                 /* 集まった数 */
static unsigned char g_mip[MESH_MAX][4];   /* 送り主（同じ相手は一度だけ） */
static char          g_mval[MESH_MAX][40]; /* 返ってきた値の文字 */

static void mesh_collect_put(const unsigned char *ip, const char *val)
{
    if (g_mn >= MESH_MAX) return;
    for (int i = 0; i < g_mn; i++)
        if (g_mip[i][0]==ip[0] && g_mip[i][1]==ip[1]
         && g_mip[i][2]==ip[2] && g_mip[i][3]==ip[3]) return;   /* 同じ相手は一つ */
    for (int k = 0; k < 4; k++) g_mip[g_mn][k] = ip[k];
    { int k = 0; if (val) while (val[k] && k < (int)sizeof g_mval[0] - 1)
        { g_mval[g_mn][k] = val[k]; k++; } g_mval[g_mn][k] = 0; }
    g_mn++;
}

static volatile int  g_wait_id = -1;          /* 待っている reqid。-1 = 待っていない */
static volatile int  g_have    = 0;
static char          g_reply[192];
static int           g_next_id = 1;

/* ---- 送信バッファ。MMU の都合で 64 境界に置く（tcp_server と同じ作法） ---- */
static volatile unsigned char __attribute__((aligned(64))) g_tx[1518];

static unsigned short ip_csum(const unsigned char *d, int n)
{
    unsigned long s = 0; int i;
    for (i = 0; i + 1 < n; i += 2) s += ((unsigned long)d[i] << 8) | d[i+1];
    if (i < n) s += (unsigned long)d[i] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (unsigned short)(~s & 0xFFFF);
}

/* IP ヘッダの識別子。固定値だと経路上で同一フラグメント扱いされうる。 */
/* ---- 本物のミリ秒 --------------------------------------------------------
   ★ timer_ticks() を「100Hz ＝ 1 刻み 10ms」として扱っていたが、実測では
     Pi 5 で 1 刻み ≒ 2ms（約 500Hz）だった。計器が示したのはこうである:
       ・200ms ごとに出し直すつもりの再送が、実際には 40ms 間隔
       ・1500ms のつもりの窓が、実際には 240ms で閉じる
     そのため「300ms 遅れて返す相手」が必ず取りこぼされ、gather が
     速い相手（Mac）しか拾えなかった。Pi 4 でも同じ（1ms 内に 3 通）。
     刻みの速さを仮定するのをやめ、汎用タイマの計数器から直に ms を作る。
     cntfrq_el0 が周波数を教えるので、板ごとの定数も要らない。 */
static unsigned long mesh_now_ms(void)
{
    unsigned long t, f;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t));
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f) f = 54000000UL;
    return t / (f / 1000UL);
}

static unsigned short g_ipid = 0x4100;

static int send_udp(const unsigned char *dmac, const unsigned char *dip,
                    unsigned short dport, unsigned short sport,
                    const char *payload, int plen)
{
    unsigned char smac[6], sip[4];
    int i, udplen = 8 + plen, iptot = 20 + udplen, framelen = 14 + iptot;
    if (plen < 0 || framelen > (int)sizeof g_tx) return -1;
    net_responder_get_mac(smac);
    net_responder_get_ip(sip);

    for (i = 0; i < 6; i++) g_tx[i]     = dmac[i];
    for (i = 0; i < 6; i++) g_tx[6 + i] = smac[i];
    g_tx[12] = 0x08; g_tx[13] = 0x00;                 /* IPv4 */

    { volatile unsigned char *p = g_tx + 14;
      for (i = 0; i < 20; i++) p[i] = 0;
      p[0] = 0x45;                                    /* v4, 5 words   */
      p[2] = (unsigned char)(iptot >> 8); p[3] = (unsigned char)(iptot & 0xFF);
      p[4] = (unsigned char)(g_ipid >> 8); p[5] = (unsigned char)(g_ipid & 0xFF);
      g_ipid++;
      p[6] = 0x40;                                    /* Don't Fragment */
      p[8] = 64;                                      /* TTL           */
      p[9] = 17;                                      /* UDP           */
      for (i = 0; i < 4; i++) p[12 + i] = sip[i];
      for (i = 0; i < 4; i++) p[16 + i] = dip[i];
      { unsigned char h[20]; for (i = 0; i < 20; i++) h[i] = p[i];
        unsigned short c = ip_csum(h, 20);
        p[10] = (unsigned char)(c >> 8); p[11] = (unsigned char)(c & 0xFF); } }

    { volatile unsigned char *u = g_tx + 34;
      u[0] = (unsigned char)(sport >> 8); u[1] = (unsigned char)(sport & 0xFF);
      u[2] = (unsigned char)(dport >> 8); u[3] = (unsigned char)(dport & 0xFF);
      u[4] = (unsigned char)(udplen >> 8); u[5] = (unsigned char)(udplen & 0xFF);
      u[6] = 0; u[7] = 0;            /* UDP チェックサム省略（IPv4 では 0 が許される） */
      for (i = 0; i < plen; i++) u[8 + i] = (unsigned char)payload[i]; }

    return genet_tx_frame((const unsigned char *)g_tx, framelen);
}

/* ---- 小さな文字列道具（機内 libc に頼らない） ---------------------------- */
static int s_eq(const char *a, const char *b)
{ int i = 0; while (a[i] && a[i] == b[i]) i++; return a[i] == 0 && b[i] == 0; }
static int put(char *d, int at, int cap, const char *s)
{ while (*s && at < cap - 1) d[at++] = *s++; d[at] = 0; return at; }
static int put_num(char *d, int at, int cap, long v)
{ char t[24]; int n = 0; if (v < 0) { if (at < cap-1) d[at++] = '-'; v = -v; }
  if (v == 0) t[n++] = '0';
  while (v > 0) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
  while (n > 0 && at < cap - 1) d[at++] = t[--n];
  d[at] = 0; return at; }

/* ---- 受信 ---------------------------------------------------------------
 * main.c の受信連鎖から呼ばれる。消費したら 1 を返す。 */
int aipl_remote_handle(const unsigned char *f, int len)
{
    unsigned char myip[4];
    if (len < 14 + 20 + 8) return 0;
    if (!(f[12] == 0x08 && f[13] == 0x00)) return 0;          /* IPv4 か */
    const unsigned char *ip = f + 14;
    if ((ip[0] >> 4) != 4) return 0;
    int ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || len < 14 + ihl + 8) return 0;
    if (ip[9] != 17) return 0;                                 /* UDP か */

    net_responder_get_ip(myip);
    /* 自分宛のほかに、同報（255.255.255.255 と 自網.255）も受ける。
       メッシュの三つ（neighbors / broadcast / gather）は宛先を書かずに撒くので、
       ここを自分宛だけに絞っていると一通も届かない。 */
    { int mine = (ip[16]==myip[0] && ip[17]==myip[1] && ip[18]==myip[2] && ip[19]==myip[3]);
      int allb = (ip[16]==255 && ip[17]==255 && ip[18]==255 && ip[19]==255);
      int subb = (ip[16]==myip[0] && ip[17]==myip[1] && ip[18]==myip[2] && ip[19]==255);
      if (!mine && !allb && !subb) return 0; }

    const unsigned char *u = f + 14 + ihl;
    unsigned short dport = (unsigned short)((u[2] << 8) | u[3]);
    unsigned short sport = (unsigned short)((u[0] << 8) | u[1]);
    if (dport != AIPL_PORT) return 0;

    int udplen = (u[4] << 8) | u[5];
    if (udplen < 8) return 1;
    int plen = udplen - 8;
    if (plen > 512) plen = 512;
    if (14 + ihl + 8 + plen > len) plen = len - (14 + ihl + 8);
    if (plen <= 0) return 1;

    char buf[544];
    for (int i = 0; i < plen && i < (int)sizeof buf - 1; i++) buf[i] = (char)u[8 + i];
    buf[(plen < (int)sizeof buf - 1) ? plen : (int)sizeof buf - 1] = 0;
    { int i = 0; while (buf[i]) { if (buf[i] == '\n' || buf[i] == '\r') { buf[i] = 0; break; } i++; } }

    peer_learn(ip + 12, f + 6);     /* 送り主の IP と MAC を覚える */

    /* ---- 応答が来た ---- */
    if (buf[0] == 'R' && buf[1] == ' ') {
        g_n_rx_r++;
        int p = 2, id = 0;
        while (buf[p] >= '0' && buf[p] <= '9') { id = id * 10 + (buf[p] - '0'); p++; }
        if (buf[p] == ' ') p++;
        /* gather(...) が収集中なら、送り主ごとに一つ積む（remote の待ちとは別） */
        if (g_mwait_id == id && g_mwait_kind == 'R') mesh_collect_put(ip + 12, buf + p);
        g_dbg_seen_id = id; g_dbg_wait_at_seen = g_wait_id;
        g_dbg_seen_t = timer_ticks();
        if (g_wait_id == id) {
            int k = 0;
            while (buf[p] && k < (int)sizeof g_reply - 1) g_reply[k++] = buf[p++];
            g_reply[k] = 0;
            g_have = 1;
            g_n_rx_match++;
        }
        return 1;
    }

    /* ---- H <id> : 誰かいますか（同報）。居ることだけを単送で返す ---- */
    if (buf[0] == 'H' && buf[1] == ' ') {
        int p = 2, id = 0;
        while (buf[p] >= '0' && buf[p] <= '9') { id = id * 10 + (buf[p] - '0'); p++; }
        char out[32]; int at = 0;
        at = put(out, at, sizeof out, "A ");
        at = put_num(out, at, sizeof out, id);
        at = put(out, at, sizeof out, "\n");
        send_udp(f + 6, ip + 12, sport, AIPL_PORT, out, at);
        return 1;
    }

    /* ---- A <id> : neighbors() の返事。送り主の IP そのものが答である ---- */
    if (buf[0] == 'A' && buf[1] == ' ') {
        int p = 2, id = 0;
        while (buf[p] >= '0' && buf[p] <= '9') { id = id * 10 + (buf[p] - '0'); p++; }
        if (g_mwait_id == id && g_mwait_kind == 'A') mesh_collect_put(ip + 12, "");
        return 1;
    }

    /* ---- B <id> <役> <メソッド> <引数> : broadcast(...)。返事は出さない ----
       返すと、撒いた一通に対して全員が返して嵐になる。控え（at-most-once）は
       Q と同じものを使うので、再送で二度走ることはない。 */
    if (buf[0] == 'B' && buf[1] == ' ') {
        int p = 2, id = 0;
        while (buf[p] >= '0' && buf[p] <= '9') { id = id * 10 + (buf[p] - '0'); p++; }
        if (buf[p] == ' ') p++;
        char actor[40]; int k = 0;
        while (buf[p] && buf[p] != ' ' && k < (int)sizeof actor - 1) actor[k++] = buf[p++];
        actor[k] = 0;
        if (buf[p] == ' ') p++;
        char meth[40]; k = 0;
        while (buf[p] && buf[p] != ' ' && k < (int)sizeof meth - 1) meth[k++] = buf[p++];
        meth[k] = 0;
        if (buf[p] == ' ') p++;
        if (ans_lookup(ip + 12, id)) return 1;          /* 再送。もう走らせた */
        { int target = -1;
          for (int i = 0; i < cc_web_count(); i++) {
              const char *pth = cc_web_path_at(i);
              if (!pth) continue;
              if (s_eq(pth, actor)) { target = cc_web_actor_at(i); break; }
              if (pth[0] == '/' && s_eq(pth + 1, actor)) { target = cc_web_actor_at(i); break; }
          }
          char val[160];
          if (target >= 0) cc_remote_dispatch(target, meth, buf + p, val, sizeof val);
          ans_store(ip + 12, id, "ok"); }
        return 1;
    }

    /* ---- 要求が来た ---- */
    if (buf[0] == 'Q' && buf[1] == ' ') {
        g_n_rx_q++;
        int p = 2, id = 0;
        while (buf[p] >= '0' && buf[p] <= '9') { id = id * 10 + (buf[p] - '0'); p++; }
        if (buf[p] == ' ') p++;
        char actor[40]; int k = 0;
        while (buf[p] && buf[p] != ' ' && k < (int)sizeof actor - 1) actor[k++] = buf[p++];
        actor[k] = 0;
        if (buf[p] == ' ') p++;
        char meth[40]; k = 0;
        while (buf[p] && buf[p] != ' ' && k < (int)sizeof meth - 1) meth[k++] = buf[p++];
        meth[k] = 0;
        if (buf[p] == ' ') p++;
        const char *arg = buf + p;                 /* 行末まで。空白を含んでよい */

        /* 公開名からアクター番号を引く。web_expose の表は "/echo" のように
           先頭が '/' なので、両方の書き方を受ける。 */
        /* 同じ要求の再送なら、走らせずに前の答えを返す */
        { const char *prev = ans_lookup(ip + 12, id);
          if (prev) {
              char out[224]; int at = 0;
              at = put(out, at, sizeof out, "R ");
              at = put_num(out, at, sizeof out, id);
              at = put(out, at, sizeof out, " ");
              at = put(out, at, sizeof out, prev);
              at = put(out, at, sizeof out, "\n");
              send_udp(f + 6, ip + 12, sport, AIPL_PORT, out, at);
              return 1;
          } }

        int target = -1;
        for (int i = 0; i < cc_web_count(); i++) {
            const char *pth = cc_web_path_at(i);
            if (!pth) continue;
            if (s_eq(pth, actor)) { target = cc_web_actor_at(i); break; }
            if (pth[0] == '/' && s_eq(pth + 1, actor)) { target = cc_web_actor_at(i); break; }
        }

        char val[160];
        if (target < 0) put(val, 0, sizeof val, "err");
        else if (cc_remote_dispatch(target, meth, arg, val, sizeof val) != 0)
            put(val, 0, sizeof val, "err");

        ans_store(ip + 12, id, val);

        char out[224]; int at = 0;
        at = put(out, at, sizeof out, "R ");
        at = put_num(out, at, sizeof out, id);
        at = put(out, at, sizeof out, " ");
        at = put(out, at, sizeof out, val);
        at = put(out, at, sizeof out, "\n");
        send_udp(f + 6, ip + 12, sport, AIPL_PORT, out, at);
        return 1;
    }
    return 1;      /* 我々の口に来たが読めない電文。捨てる */
}

/* ---- 送信 ---------------------------------------------------------------
 * "192.168.3.100" または "192.168.3.100:9010" を解く。 */
static int parse_hostport(const char *h, unsigned char ip[4], unsigned short *port)
{
    int i, v, n = 0;
    *port = AIPL_PORT;
    for (i = 0; i < 4; i++) {
        v = 0; if (!(h[n] >= '0' && h[n] <= '9')) return -1;
        while (h[n] >= '0' && h[n] <= '9') { v = v * 10 + (h[n] - '0'); n++; }
        if (v > 255) return -1;
        ip[i] = (unsigned char)v;
        if (i < 3) { if (h[n] != '.') return -1; n++; }
    }
    if (h[n] == ':') { n++; v = 0;
        while (h[n] >= '0' && h[n] <= '9') { v = v * 10 + (h[n] - '0'); n++; }
        if (v > 0 && v < 65536) *port = (unsigned short)v; }
    return 0;
}

static int build_q(char *out, int cap, int id, const char *actor,
                   const char *meth, const char *arg)
{
    int at = 0;
    at = put(out, at, cap, "Q ");
    at = put_num(out, at, cap, id);
    at = put(out, at, cap, " ");
    at = put(out, at, cap, actor);
    at = put(out, at, cap, " ");
    at = put(out, at, cap, meth);
    at = put(out, at, cap, " ");
    at = put(out, at, cap, arg ? arg : "");
    at = put(out, at, cap, "\n");
    return at;
}

/* 返事を待たない送信（send remote(...).m(a)）。 */
int aipl_remote_send(const char *hostport, const char *actor,
                     const char *meth, const char *arg)
{
    unsigned char ip[4], mac[6]; unsigned short port;
    char q[288]; int n;
    if (parse_hostport(hostport, ip, &port) != 0) return -1;
    if (!peer_lookup(ip, mac)) for (int i = 0; i < 6; i++) mac[i] = 0xFF;  /* 撒く */
    n = build_q(q, sizeof q, g_next_id++, actor, meth, arg);
    return send_udp(mac, ip, port, AIPL_PORT, q, n);
}

/* 返事を待つ送信（now remote(...).m(a) timeout ms）。
 * 応答は main.c の受信ループが aipl_remote_handle で置いていく。したがって
 * ここで待つ側は必ず「譲る」こと ―― 塞いで回すと受信ループが動かず、
 * 応答は永遠に来ない。 */
int aipl_remote_call(const char *hostport, const char *actor, const char *meth,
                     const char *arg, long timeout_ms, char *out, int outcap)
{
    extern void proc_sleep_us(unsigned long us);
    extern void net_rx_pump(void);              /* 待つ側が自分で受信を回す */
    extern void net_rx_remote_window(int on);   /* 割り込みにも汲ませる窓 */
    unsigned char ip[4], mac[6]; unsigned short port;
    char q[288]; int n, id;
    unsigned long t0;

    if (outcap > 0) out[0] = 0;
    if (parse_hostport(hostport, ip, &port) != 0) return -1;
    if (!peer_lookup(ip, mac)) for (int i = 0; i < 6; i++) mac[i] = 0xFF;

    id = g_next_id++;
    if (g_next_id > 1000000) g_next_id = 1;
    n = build_q(q, sizeof q, id, actor, meth, arg);

    g_have = 0; g_wait_id = id;
    if (send_udp(mac, ip, port, AIPL_PORT, q, n) < 0) { g_wait_id = -1; return -1; }
    g_n_tx_q++;
    g_dbg_send_t = timer_ticks(); g_dbg_loops = 0;

    if (timeout_ms <= 0) timeout_ms = 2000;
    t0 = mesh_now_ms();          /* ★ 以前は刻み数を 100Hz と仮定していた */
    /* ★ UDP の要求応答なのに再送していなかった ―― これが「応答は届いている
       のに拾えない」の実際の中身だった。計器いわく、待ちの輪は 1 秒に数回しか
       回らず、環からの取り出しも遅れる。落ちた一発をそのまま待てば必ず期限切れ
       になる。相手に繰り返し返させたら通った、という実測がそれを示した。
       200ms ごとに出し直す。相手は (送り主, reqid) の控えを持つので、
       メソッドが二度走ることはない。 */
    net_rx_remote_window(1);       /* ここから割り込みも環を汲む（remote だけ配る） */
    { unsigned long last_tx = t0;
      while (!g_have) {
          if ((long)(mesh_now_ms() - t0) >= timeout_ms) {
              g_wait_id = -1; g_n_timeout++; net_rx_remote_window(0); return -2; }
          g_dbg_loops++;
          net_rx_pump();
          if (mesh_now_ms() - last_tx >= 200) {         /* 200ms ごと */
              send_udp(mac, ip, port, AIPL_PORT, q, n);
              last_tx = mesh_now_ms();
              g_n_retx++;
          }
          proc_sleep_us(2000);       /* 譲る。塞がない */
      } }
    net_rx_remote_window(0);
    g_wait_id = -1;
    { int k = 0; while (g_reply[k] && k < outcap - 1) { out[k] = g_reply[k]; k++; } out[k] = 0; }
    return 0;
}

/* ===== メッシュの三つ（送り手） ============================================
 * 宛先表を持たない。自網の同報 (自網.255 / ff:ff:ff:ff:ff:ff) へ撒いて、
 * 返ってきた分だけを拾う。経路が変わっても、誰が消えても、ここは変わらない。
 * 電文は remote(...) と同じ UDP/9010 の一行 ASCII である。 */

static void mesh_bcast_ip(unsigned char dip[4])
{
    net_responder_get_ip(dip);
    dip[3] = 255;                       /* 自網の同報 */
}

/* 撒いて、期限まで待って、集まった分を返す。待つ間は必ず譲る
 * （塞ぐと受信ループが動かず、返事は永遠に来ない ―― remote と同じ轍）。 */
static int mesh_run(const char *line, int len, int id, char kind, long ms)
{
    extern void proc_sleep_us(unsigned long us);
    extern void net_rx_pump(void);
    extern void net_rx_remote_window(int on);
    unsigned char dip[4], bmac[6];
    unsigned long t0, last_tx;      /* ミリ秒（刻み数ではない）*/
    for (int i = 0; i < 6; i++) bmac[i] = 0xFF;
    mesh_bcast_ip(dip);

    g_mn = 0; g_mwait_kind = kind; g_mwait_id = id;
    net_rx_remote_window(1);
    send_udp(bmac, dip, AIPL_PORT, AIPL_PORT, line, len);
    t0 = mesh_now_ms(); last_tx = t0;
    if (ms <= 0) ms = 1;
    while ((long)(mesh_now_ms() - t0) < ms) {
        net_rx_pump();
        if (mesh_now_ms() - last_tx >= 200) {          /* 200ms ごとに出し直す */
            send_udp(bmac, dip, AIPL_PORT, AIPL_PORT, line, len);
            last_tx = mesh_now_ms();
        }
        proc_sleep_us(2000);
        if (g_mn >= MESH_MAX) break;
    }
    net_rx_remote_window(0);
    g_mwait_id = -1; g_mwait_kind = 0;
    return g_mn;
}

/* neighbors() — 今つながっている相手の IP。過去に見た相手ではない。 */
int aipl_mesh_probe(char *out, int stride, int max, long ms)
{
    char q[32]; int n = 0, id = g_next_id++;
    if (g_next_id > 1000000) g_next_id = 1;
    n = put(q, n, sizeof q, "H ");
    n = put_num(q, n, sizeof q, id);
    n = put(q, n, sizeof q, "\n");
    mesh_run(q, n, id, 'A', ms > 0 ? ms : 300);
    { int k = 0;
      for (int i = 0; i < g_mn && k < max; i++) {
          char *d = out + (long)k * stride; int at = 0;
          for (int j = 0; j < 4; j++) {
              at = put_num(d, at, stride, g_mip[i][j]);
              if (j < 3) at = put(d, at, stride, ".");
          }
          k++;
      }
      return k; }
}

/* broadcast(役, メソッド, 引数) — 撒くだけ。返事は求めない。 */
int aipl_mesh_bcast(const char *actor, const char *meth, const char *arg)
{
    unsigned char dip[4], bmac[6];
    char q[288]; int n = 0, id = g_next_id++;
    if (g_next_id > 1000000) g_next_id = 1;
    for (int i = 0; i < 6; i++) bmac[i] = 0xFF;
    mesh_bcast_ip(dip);
    n = put(q, n, sizeof q, "B ");
    n = put_num(q, n, sizeof q, id);
    n = put(q, n, sizeof q, " ");
    n = put(q, n, sizeof q, actor);
    n = put(q, n, sizeof q, " ");
    n = put(q, n, sizeof q, meth);
    n = put(q, n, sizeof q, " ");
    n = put(q, n, sizeof q, arg ? arg : "");
    n = put(q, n, sizeof q, "\n");
    return send_udp(bmac, dip, AIPL_PORT, AIPL_PORT, q, n);
}

/* gather(役, メソッド, 引数, ms) — 同報で問い、期限までに届いた分だけ返す。
 * 全員から返る保証は無い。だから戻りが配列なのである。 */
int aipl_mesh_gather(const char *actor, const char *meth, const char *arg,
                     long ms, char *out, int stride, int max)
{
    char q[288]; int n, id = g_next_id++;
    if (g_next_id > 1000000) g_next_id = 1;
    n = build_q(q, sizeof q, id, actor, meth, arg);
    mesh_run(q, n, id, 'R', ms);
    { int k = 0;
      for (int i = 0; i < g_mn && k < max; i++) {
          char *d = out + (long)k * stride;
          int j = 0; while (g_mval[i][j] && j < stride - 1) { d[j] = g_mval[i][j]; j++; }
          d[j] = 0; k++;
      }
      return k; }
}
