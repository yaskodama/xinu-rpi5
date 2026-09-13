/* system/browser.c —— 機内ブラウザ
 *
 * airilab.app のような外部サイトを板から直接読むための最小構成。
 * この板には**外向き通信の道具が 3 つとも無かった**ので、そこから作る。
 *
 *   ① ARP 要求 —— 既存の ARP は「要求してきた相手へ返す」受け身だけで、
 *      こちらから引く機能が無い。LAN の外へ出るにはゲートウェイの MAC が要る。
 *   ② 経路判定 —— 宛先がサブネット外なら、宛先ではなく**ゲートウェイ**の MAC へ送る。
 *      既存の唯一の外向き路（aipl_remote）は「受信フレームの送り主 MAC へ返す」
 *      passive learning なので、こちらから話しかける相手には使えない。
 *   ③ TCP の能動オープン —— 既存の TCP は待ち受けのみ。SYN を送る側が無い。
 *
 * TLS は実装しない。airilab.app は平文 HTTP でも 200 を返すことを確認済み。
 *
 * フレーム組み立てはすべて volatile で 1 バイトずつ書く。MMU の設定次第で
 * DRAM が Device-nGnRnE になり、GCC がまとめた stp/strh が未整列で落ちるため
 * （net_responder.c の gratuitous ARP に同じ注記がある）。
 */

extern int  genet_tx_frame(const unsigned char *frame, int length);
extern void net_responder_get_mac(unsigned char out[6]);
extern void net_responder_get_ip(unsigned char out[4]);
/* ---- 無線（メッシュ）側。宛先が WiFi の副網なら、こちらの口で出す ----
 * 有線は airilab.app などの外向き、無線はメッシュ上の他の Xinu 板（10.0.0.n）。
 * 送信口と自分の IP/MAC を宛先ごとに選ぶ。受信は wifi_handle_frame が
 * browser_handle を先に呼ぶので、待ち方は有線と同じ。 */
extern int  wifi_connected(void);
extern int  wifi_eth_tx(const unsigned char *eth, int len);
extern void wifi_ipaddr(unsigned char *o);
extern void wifi_netmask(unsigned char *o);
extern void wifi_macaddr(unsigned char *o);
extern void wifi_net_poll(void);
static int  br_via_wifi = 0;                 /* いまの宛先を無線で出すか（br_route_mac が決める） */
static int  br_wifi_has(const unsigned char *dip)   /* dip は WiFi の副網か */
{
    if (!wifi_connected()) return 0;
    unsigned char ip[4], m[4]; wifi_ipaddr(ip); wifi_netmask(m);
    if (!(ip[0] | ip[1] | ip[2] | ip[3])) return 0;
    if (!(m[0] | m[1] | m[2] | m[3])) { m[0] = m[1] = m[2] = 255; m[3] = 0; }
    for (int i = 0; i < 4; i++) if ((unsigned char)(ip[i] & m[i]) != (unsigned char)(dip[i] & m[i])) return 0;
    return 1;
}
static void br_src_mac(unsigned char *o) { if (br_via_wifi) wifi_macaddr(o); else net_responder_get_mac(o); }
static void br_src_ip(unsigned char *o)  { if (br_via_wifi) wifi_ipaddr(o);  else net_responder_get_ip(o); }
static int  br_link_tx(const unsigned char *f, int n) { return br_via_wifi ? wifi_eth_tx(f, n) : genet_tx_frame(f, n); }
extern void dhcp_get_router(unsigned char out[4]);
extern void dhcp_get_netmask(unsigned char out[4]);
extern void dhcp_get_dns(unsigned char out[4]);
extern void uart_puts(const char *s);
extern void proc_sleep_us(unsigned long us);
extern void net_rx_pump(void);      /* 受信環を自分で汲む（下の注記を見よ） */

static unsigned long br_now_us(void);

/* ★ 待ち方について。
 * この板では HTTP 要求は genet_rx_tick() の **中** で処理される。
 * そこで proc_sleep_us() で寝ると **受信の巡回ごと止まり**、
 * ARP 応答も DNS 応答も TCP も永久に処理されない
 * （最初の実装はこれで「経路がありません」を返し続けた）。
 * 待つあいだは自分で環を汲む。aipl_remote が同じ問題に同じ対処をしている。 */
static void br_wait_us(unsigned long us)
{
    unsigned long t0 = br_now_us();
    do { net_rx_pump(); if (br_via_wifi) wifi_net_poll(); } while ((long)(br_now_us() - t0) < (long)us);
}

static unsigned long br_now_us(void)
{
    unsigned long ct, hz;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(ct));
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(hz));
    return hz ? (ct * 1000000UL) / hz : 0;
}

/* ===== 小さな道具（機内 libc に頼らない） ============================== */
static int  b_len(const char *s){ int n=0; while(s[n]) n++; return n; }
static int  b_eqn(const char *a, const char *b, int n)
{ for (int i=0;i<n;i++){ char x=a[i],y=b[i];
    if (x>='A'&&x<='Z') x=(char)(x+32); if (y>='A'&&y<='Z') y=(char)(y+32);
    if (x!=y) return 0; } return 1; }
static void b_cpy(char *d, const char *s, int cap)
{ int i=0; while (s[i] && i<cap-1){ d[i]=s[i]; i++; } d[i]=0; }

/* ===== 送受信の共有バッファ ============================================ */
#define BR_TXMAX 1600
static volatile unsigned char __attribute__((aligned(16))) br_tx[BR_TXMAX];
static unsigned short br_ipid = 0x4200;

static unsigned short br_csum16(const unsigned char *d, int n, unsigned long seed)
{
    unsigned long s = seed; int i;
    for (i = 0; i + 1 < n; i += 2) s += ((unsigned long)d[i] << 8) | d[i+1];
    if (i < n) s += (unsigned long)d[i] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (unsigned short)(~s & 0xFFFF);
}

/* ===== ① ARP 要求と学習 ================================================ */
static struct { unsigned char ip[4], mac[6], used; } br_arp[8];

static void br_arp_learn(const unsigned char *ip, const unsigned char *mac)
{
    int free_slot = -1;
    for (int i = 0; i < 8; i++) {
        if (br_arp[i].used) {
            int same = 1;
            for (int k = 0; k < 4; k++) if (br_arp[i].ip[k] != ip[k]) same = 0;
            if (same) { for (int k = 0; k < 6; k++) br_arp[i].mac[k] = mac[k]; return; }
        } else if (free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) free_slot = 0;
    for (int k = 0; k < 4; k++) br_arp[free_slot].ip[k]  = ip[k];
    for (int k = 0; k < 6; k++) br_arp[free_slot].mac[k] = mac[k];
    br_arp[free_slot].used = 1;
}

static int br_arp_lookup(const unsigned char *ip, unsigned char *mac)
{
    for (int i = 0; i < 8; i++) {
        if (!br_arp[i].used) continue;
        int same = 1;
        for (int k = 0; k < 4; k++) if (br_arp[i].ip[k] != ip[k]) same = 0;
        if (same) { for (int k = 0; k < 6; k++) mac[k] = br_arp[i].mac[k]; return 1; }
    }
    return 0;
}

static void br_arp_request(const unsigned char *tip)
{
    unsigned char smac[6], sip[4];
    br_src_mac(smac); br_src_ip(sip);
    for (int i = 0; i < 60; i++) br_tx[i] = 0;
    for (int i = 0; i < 6; i++) br_tx[i] = 0xFF;            /* 宛先 = 同報 */
    for (int i = 0; i < 6; i++) br_tx[6+i] = smac[i];
    br_tx[12] = 0x08; br_tx[13] = 0x06;                     /* ARP */
    br_tx[14] = 0x00; br_tx[15] = 0x01;                     /* Ethernet */
    br_tx[16] = 0x08; br_tx[17] = 0x00;                     /* IPv4 */
    br_tx[18] = 6; br_tx[19] = 4;
    br_tx[20] = 0x00; br_tx[21] = 0x01;                     /* request */
    for (int i = 0; i < 6; i++) br_tx[22+i] = smac[i];
    for (int i = 0; i < 4; i++) br_tx[28+i] = sip[i];
    for (int i = 0; i < 4; i++) br_tx[38+i] = tip[i];
    br_link_tx((const unsigned char *)br_tx, 60);
}

/* 相手の MAC を引く。要求は落ちうるので再送する（UDP と同じ作法）。 */
static int br_arp_resolve(const unsigned char *ip, unsigned char *mac, int ms)
{
    if (br_arp_lookup(ip, mac)) return 1;
    unsigned long t0 = br_now_us();
    int tries = 0;
    while ((long)(br_now_us() - t0) < (long)ms * 1000L) {
        if ((tries++ % 40) == 0) br_arp_request(ip);        /* 200ms ごとに再送 */
        br_wait_us(5000);
        if (br_arp_lookup(ip, mac)) return 1;
    }
    return 0;
}

/* ===== 経路の設定 ======================================================
 * この板は **DHCP を意図的に使っていない**（loader/main.c: 静的 IP 192.168.3.101
 * で立ち上げ、DHCP は arm しない）。したがって dhcp_get_netmask/router/dns は
 * すべて 0 を返す ―― 最初の実装はこれを信じて「経路がありません」を返し続けた。
 * DHCP が値を持っていればそれを使い、無ければ既定値で補い、
 * /browse?gw=&dns= で実行時に上書きできるようにする。 */
static unsigned char br_cfg_gw[4]  = {0,0,0,0};   /* 0 なら自分の /24 の .1 */
static unsigned char br_cfg_dns[4] = {0,0,0,0};   /* 0 ならゲートウェイを使う */

static int br_zero4(const unsigned char *a)
{ return a[0]==0 && a[1]==0 && a[2]==0 && a[3]==0; }

static void br_get_mask(unsigned char *m)
{
    dhcp_get_netmask(m);
    if (br_zero4(m)) { m[0]=255; m[1]=255; m[2]=255; m[3]=0; }   /* 既定 /24 */
}
static void br_get_gw(unsigned char *g)
{
    if (!br_zero4(br_cfg_gw)) { for (int i=0;i<4;i++) g[i]=br_cfg_gw[i]; return; }
    dhcp_get_router(g);
    if (br_zero4(g)) {                       /* 自分の /24 の .1 を既定にする */
        unsigned char ip[4]; net_responder_get_ip(ip);
        g[0]=ip[0]; g[1]=ip[1]; g[2]=ip[2]; g[3]=1;
    }
}
static void br_get_dns(unsigned char *d)
{
    if (!br_zero4(br_cfg_dns)) { for (int i=0;i<4;i++) d[i]=br_cfg_dns[i]; return; }
    dhcp_get_dns(d);
    if (br_zero4(d)) br_get_gw(d);           /* 家庭用ルータは DNS も兼ねる */
}
/* "a.b.c.d" を 4 バイトに。成功なら 1。 */
int browser_parse_ip(const char *s, unsigned char *out)
{
    int v=0, d=0, k=0;
    for (int i=0; ; i++) {
        char c = s[i];
        if (c>='0' && c<='9') { v = v*10 + (c-'0'); d=1; }
        else if (c=='.') { if(!d||k>2||v>255) return 0; out[k++]=(unsigned char)v; v=0; d=0; }
        else if (c==0 || c=='&' || c==' ') { if(!d||k!=3||v>255) return 0; out[3]=(unsigned char)v; return 1; }
        else return 0;
    }
}
void browser_set_gw(const unsigned char *g)  { for(int i=0;i<4;i++) br_cfg_gw[i]=g[i]; }
void browser_set_dns(const unsigned char *d) { for(int i=0;i<4;i++) br_cfg_dns[i]=d[i]; }

/* ===== ② 経路判定 ====================================================== */
/* 宛先がサブネット外なら、ゲートウェイの MAC へ送る。 */
static int br_route_mac(const unsigned char *dip, unsigned char *mac)
{
    unsigned char sip[4], mask[4], gw[4];
    if (br_wifi_has(dip)) { br_via_wifi = 1; return br_arp_resolve(dip, mac, 2000); }   /* メッシュ上の板：無線で直接 */
    br_via_wifi = 0;
    net_responder_get_ip(sip); br_get_mask(mask); br_get_gw(gw);
    int onlink = 1;
    for (int i = 0; i < 4; i++)
        if ((unsigned char)(sip[i] & mask[i]) != (unsigned char)(dip[i] & mask[i])) onlink = 0;
    const unsigned char *target = onlink ? dip : gw;
    if (!onlink && br_zero4(gw)) return 0;
    return br_arp_resolve(target, mac, 2000);
}

/* ===== IP/UDP/TCP の送信 =============================================== */
static int br_ip_send(const unsigned char *dmac, const unsigned char *dip,
                      int proto, const unsigned char *pl, int plen)
{
    unsigned char smac[6], sip[4];
    int iptot = 20 + plen, framelen = 14 + iptot;
    if (plen < 0 || framelen > BR_TXMAX) return -1;
    br_src_mac(smac); br_src_ip(sip);
    for (int i = 0; i < 6; i++) br_tx[i]     = dmac[i];
    for (int i = 0; i < 6; i++) br_tx[6 + i] = smac[i];
    br_tx[12] = 0x08; br_tx[13] = 0x00;
    { volatile unsigned char *p = br_tx + 14;
      for (int i = 0; i < 20; i++) p[i] = 0;
      p[0] = 0x45;
      p[2] = (unsigned char)(iptot >> 8); p[3] = (unsigned char)(iptot & 0xFF);
      p[4] = (unsigned char)(br_ipid >> 8); p[5] = (unsigned char)(br_ipid & 0xFF); br_ipid++;
      p[6] = 0x40; p[8] = 64; p[9] = (unsigned char)proto;
      for (int i = 0; i < 4; i++) p[12 + i] = sip[i];
      for (int i = 0; i < 4; i++) p[16 + i] = dip[i];
      { unsigned char h[20]; for (int i = 0; i < 20; i++) h[i] = p[i];
        unsigned short c = br_csum16(h, 20, 0);
        p[10] = (unsigned char)(c >> 8); p[11] = (unsigned char)(c & 0xFF); } }
    { volatile unsigned char *q = br_tx + 34;
      for (int i = 0; i < plen; i++) q[i] = pl[i]; }
    if (framelen < 60) { for (int i = framelen; i < 60; i++) br_tx[i] = 0; framelen = 60; }
    return br_link_tx((const unsigned char *)br_tx, framelen);
}

/* 擬似ヘッダ込みのチェックサム（UDP/TCP 共通）。 */
static unsigned short br_l4_csum(const unsigned char *sip, const unsigned char *dip,
                                 int proto, const unsigned char *seg, int n)
{
    unsigned long s = 0;
    s += ((unsigned long)sip[0] << 8) | sip[1];  s += ((unsigned long)sip[2] << 8) | sip[3];
    s += ((unsigned long)dip[0] << 8) | dip[1];  s += ((unsigned long)dip[2] << 8) | dip[3];
    s += (unsigned long)proto;                   s += (unsigned long)n;
    return br_csum16(seg, n, s);
}

/* ===== ③ DNS ============================================================ */
static volatile int  br_dns_got;
static unsigned char br_dns_ip[4];
static unsigned short br_dns_id = 0x1234;

static int br_dns_query(const char *name, unsigned char *out)
{
    unsigned char dns[4], gwmac[6];
    br_get_dns(dns);
    if (br_zero4(dns)) return 0;
    if (!br_route_mac(dns, gwmac)) return 0;

    unsigned char q[512]; int n = 0;
    br_dns_id++;
    q[n++] = (unsigned char)(br_dns_id >> 8); q[n++] = (unsigned char)(br_dns_id & 0xFF);
    q[n++] = 0x01; q[n++] = 0x00;              /* 再帰要求 */
    q[n++] = 0; q[n++] = 1;                    /* QDCOUNT=1 */
    q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    { int i = 0; while (name[i]) {             /* ラベル列に直す */
        int st = i; while (name[i] && name[i] != '.') i++;
        q[n++] = (unsigned char)(i - st);
        for (int k = st; k < i; k++) q[n++] = (unsigned char)name[k];
        if (name[i] == '.') i++; }
      q[n++] = 0; }
    q[n++] = 0; q[n++] = 1;                    /* QTYPE=A */
    q[n++] = 0; q[n++] = 1;                    /* QCLASS=IN */

    unsigned char seg[600]; int sl = 8 + n;
    unsigned short sport = 40000 + (br_dns_id & 0x3FF);
    seg[0] = (unsigned char)(sport >> 8); seg[1] = (unsigned char)(sport & 0xFF);
    seg[2] = 0; seg[3] = 53;
    seg[4] = (unsigned char)(sl >> 8); seg[5] = (unsigned char)(sl & 0xFF);
    seg[6] = 0; seg[7] = 0;                    /* UDP チェックサム省略 */
    for (int i = 0; i < n; i++) seg[8 + i] = q[i];

    br_dns_got = 0;
    for (int t = 0; t < 4 && !br_dns_got; t++) {   /* UDP なので再送する */
        br_ip_send(gwmac, dns, 17, seg, sl);
        for (int w = 0; w < 100 && !br_dns_got; w++) br_wait_us(10000);
    }
    if (!br_dns_got) return 0;
    for (int i = 0; i < 4; i++) out[i] = br_dns_ip[i];
    return 1;
}

/* ===== TCP クライアント ================================================= */
#define BR_RXCAP 65536
static char br_page[BR_RXCAP];
static volatile int br_page_len;

static struct {
    volatile int  state;            /* 0=閉 1=SYN送信済 2=確立 3=相手FIN */
    unsigned char rip[4], rmac[6];
    unsigned short lport, rport;
    unsigned long  snd_nxt, rcv_nxt;
    volatile int   established, finished, reset;
} br_c;

static int br_tcp_send(unsigned char flags, const unsigned char *data, int dlen)
{
    unsigned char sip[4]; br_src_ip(sip);
    unsigned char seg[1500]; int hl = (flags == 0x02) ? 24 : 20;   /* SYN には MSS 選択肢を付ける */
    if (dlen < 0 || dlen > 1400) return -1;
    for (int i = 0; i < hl; i++) seg[i] = 0;
    if (hl == 24) { seg[20] = 2; seg[21] = 4; seg[22] = 0x05; seg[23] = 0xB4; }   /* MSS 1460。無いと相手は 536 で送り、段数が 3 倍になる */
    seg[0] = (unsigned char)(br_c.lport >> 8); seg[1] = (unsigned char)(br_c.lport & 0xFF);
    seg[2] = (unsigned char)(br_c.rport >> 8); seg[3] = (unsigned char)(br_c.rport & 0xFF);
    seg[4] = (unsigned char)(br_c.snd_nxt >> 24); seg[5] = (unsigned char)(br_c.snd_nxt >> 16);
    seg[6] = (unsigned char)(br_c.snd_nxt >> 8);  seg[7] = (unsigned char)(br_c.snd_nxt);
    seg[8] = (unsigned char)(br_c.rcv_nxt >> 24); seg[9] = (unsigned char)(br_c.rcv_nxt >> 16);
    seg[10]= (unsigned char)(br_c.rcv_nxt >> 8);  seg[11]= (unsigned char)(br_c.rcv_nxt);
    seg[12] = (unsigned char)((hl / 4) << 4);   /* データオフセット */
    seg[13] = flags;
    seg[14] = 0x0B; seg[15] = 0x68;       /* 窓 2920（2 段）。8192 だと一度に 5〜6 段が来て段が欠けた */
    for (int i = 0; i < dlen; i++) seg[hl + i] = data[i];
    { unsigned short c = br_l4_csum(sip, br_c.rip, 6, seg, hl + dlen);
      seg[16] = (unsigned char)(c >> 8); seg[17] = (unsigned char)(c & 0xFF); }
    return br_ip_send(br_c.rmac, br_c.rip, 6, seg, hl + dlen);
}

/* 計器: 受信した段・順序外の段・順序外の FIN・途中で切れた本文（/browse の net= に出す）。
   「本文が途中で切れる」がどこで起きているかを、推測でなく数で見るため。 */
static long br_st_seg, br_st_ooo, br_st_finooo, br_st_trunc, br_st_retry;
static long br_st_dup, br_st_fut;     /* ooo の内訳: 既に受けた段の再送（=こちらの ACK が届いていない）／先の段（=手前の段が欠けた） */
static unsigned long br_st_gem_rre, br_st_gem_ovr, br_st_gem_frames;   /* GEM 統計の累計 */

/* ===== 受信フック（main.c の連鎖の先頭に入れる） ======================== */
int browser_handle(const unsigned char *f, int len)
{
    if (len < 14) return 0;
    unsigned short et = (unsigned short)((f[12] << 8) | f[13]);

    if (et == 0x0806 && len >= 42) {                 /* ARP 応答を学習する */
        if (f[20] == 0x00 && f[21] == 0x02) br_arp_learn(f + 28, f + 22);
        return 0;                                    /* 既存の応答器にも渡す */
    }
    if (et != 0x0800 || len < 34) return 0;
    const unsigned char *ip = f + 14;
    int ihl = (ip[0] & 0x0F) * 4; if (ihl < 20) return 0;
    const unsigned char *l4 = ip + ihl;

    if (ip[9] == 17) {                               /* UDP: DNS 応答か */
        if (len < 14 + ihl + 8) return 0;
        if (!(l4[2] == 0 && l4[3] == 53)) {          /* 送り元ポートが 53 */
            if (!(l4[0] == 0 && l4[1] == 53)) return 0;
        }
        const unsigned char *d = l4 + 8;
        int dl = len - (14 + ihl + 8); if (dl < 12) return 0;
        int qd = (d[4] << 8) | d[5], an = (d[6] << 8) | d[7];
        if (an < 1) return 0;
        int p = 12;
        for (int i = 0; i < qd && p < dl; i++) {     /* 質問部を読み飛ばす */
            while (p < dl && d[p]) { if ((d[p] & 0xC0) == 0xC0) { p += 2; break; } p += d[p] + 1; }
            if (p < dl && d[p] == 0) p++;
            p += 4;
        }
        for (int i = 0; i < an && p + 12 <= dl; i++) {
            if ((d[p] & 0xC0) == 0xC0) p += 2;
            else { while (p < dl && d[p]) p += d[p] + 1; p++; }
            if (p + 10 > dl) return 0;
            int type = (d[p] << 8) | d[p+1];
            int rdl  = (d[p+8] << 8) | d[p+9];
            p += 10;
            if (type == 1 && rdl == 4 && p + 4 <= dl) {   /* A レコード */
                for (int k = 0; k < 4; k++) br_dns_ip[k] = d[p + k];
                br_dns_got = 1;
                return 1;
            }
            p += rdl;
        }
        return 0;
    }

    if (ip[9] == 6) {                                /* TCP: 自分の接続か */
        if (len < 14 + ihl + 20) return 0;
        unsigned short dp = (unsigned short)((l4[2] << 8) | l4[3]);
        if (br_c.state == 0 || dp != br_c.lport) return 0;
        unsigned short sp = (unsigned short)((l4[0] << 8) | l4[1]);
        if (sp != br_c.rport) return 0;
        unsigned long seq = ((unsigned long)l4[4] << 24) | ((unsigned long)l4[5] << 16)
                          | ((unsigned long)l4[6] << 8)  | l4[7];
        unsigned long ack = ((unsigned long)l4[8] << 24) | ((unsigned long)l4[9] << 16)
                          | ((unsigned long)l4[10] << 8) | l4[11];
        int thl = ((l4[12] >> 4) & 0x0F) * 4; if (thl < 20) thl = 20;
        unsigned char fl = l4[13];
        int dlen = (((ip[2] << 8) | ip[3]) - ihl) - thl;
        if (dlen < 0) dlen = 0;

        if (fl & 0x04) { br_c.reset = 1; br_c.state = 0; return 1; }   /* RST */

        if (br_c.state == 1 && (fl & 0x12) == 0x12) {                  /* SYN|ACK */
            br_c.rcv_nxt = seq + 1;
            br_c.snd_nxt = ack;
            br_c.state = 2; br_c.established = 1;
            br_tcp_send(0x10, 0, 0);                                   /* ACK */
            return 1;
        }
        if (br_c.state >= 2) {
            /* 順序どおりの段だけ受ける。それ以外（欠けの後に届いた段・重複）は
               同じ ACK を返して、相手に欠けた段を早く送り直させる。
               実機では 60 秒ごとの取得で本文が 3,304→2,186 文字に縮むことがあった
               ＝段が 1 つ欠けたまま FIN を受けて「完了」にしていた。 */
            if (dlen > 0 && seq != br_c.rcv_nxt) {
                br_st_ooo++;
                if ((int)(unsigned int)(seq - br_c.rcv_nxt) < 0) br_st_dup++; else br_st_fut++;   /* 32 ビットで巻く */
                br_tcp_send(0x10, 0, 0); return 1; }
            if (dlen > 0 && seq == br_c.rcv_nxt) {
                br_st_seg++;
                const unsigned char *d = l4 + thl;
                for (int i = 0; i < dlen; i++) {
                    int at = br_page_len + i;
                    if (at < BR_RXCAP - 1) br_page[at] = (char)d[i];
                }
                br_page_len += dlen;
                if (br_page_len > BR_RXCAP - 1) br_page_len = BR_RXCAP - 1;
                br_c.rcv_nxt = seq + (unsigned long)dlen;
                br_tcp_send(0x10, 0, 0);                               /* ACK */
            }
            if ((fl & 0x01) && seq + (unsigned long)dlen != br_c.rcv_nxt) br_st_finooo++;
            if ((fl & 0x01) && seq + (unsigned long)dlen == br_c.rcv_nxt) {   /* FIN（順序どおりの時だけ） */
                br_c.rcv_nxt += 1;
                br_tcp_send(0x11, 0, 0);                               /* FIN|ACK */
                br_c.state = 3; br_c.finished = 1;
            }
            return 1;
        }
        return 1;
    }
    return 0;
}

/* ===== HTTP GET ========================================================= */
static int br_port = 80;             /* 直近の URL の :port（無ければ 80） */
static int br_http_get(const unsigned char *ip, const char *host, const char *path, int ms)
{
    unsigned char mac[6];
    if (!br_route_mac(ip, mac)) return -1;

    br_page_len = 0; br_page[0] = 0;
    for (int i = 0; i < 4; i++) br_c.rip[i] = ip[i];
    for (int i = 0; i < 6; i++) br_c.rmac[i] = mac[i];
    br_c.rport = (unsigned short)(br_port > 0 ? br_port : 80);   /* URL の :port（Pi 3 の 8080 など） */
    br_c.lport = (unsigned short)(49152 + (br_now_us() & 0x3FFF));
    br_c.snd_nxt = (br_now_us() & 0x7FFFFFFF);
    br_c.rcv_nxt = 0;
    br_c.established = br_c.finished = br_c.reset = 0;
    br_c.state = 1;

    unsigned long t0 = br_now_us();
    for (int t = 0; t < 5 && !br_c.established; t++) {     /* SYN は落ちうる */
        br_tcp_send(0x02, 0, 0);                           /* SYN */
        br_c.snd_nxt += 0;                                  /* SYN の 1 は SYN|ACK 受信時に反映 */
        for (int w = 0; w < 60 && !br_c.established; w++) br_wait_us(10000);
        if (br_c.reset) return -2;
    }
    if (!br_c.established) { br_c.state = 0; return -3; }

    char req[512]; int n = 0;
    const char *g = "GET ";           for (int i=0; g[i]; i++) req[n++] = g[i];
    for (int i = 0; path[i]; i++)     req[n++] = path[i];
    const char *h = " HTTP/1.1\r\nHost: "; for (int i=0; h[i]; i++) req[n++] = h[i];
    for (int i = 0; host[i]; i++)     req[n++] = host[i];
    /* airilab.app はこの UA を見て来訪を記録し、X-Xinu-Board で板を見分ける
       （同じ家の板は同じ公開 IP になるので、IP だけでは区別できない）。 */
    const char *e = "\r\nUser-Agent: XinuBrowser/1.0\r\nX-Xinu-Board: Pi5 build ";
    for (int i = 0; e[i]; i++)        req[n++] = e[i];
    { extern const char *kernel_build_id(void); const char *b = kernel_build_id();
      for (int i = 0; b[i] && n < 440; i++) req[n++] = b[i]; }
    const char *e2 = "\r\nAccept: text/html\r\nConnection: close\r\n\r\n";
    for (int i = 0; e2[i]; i++)       req[n++] = e2[i];

    br_tcp_send(0x18, (const unsigned char *)req, n);      /* PSH|ACK */
    br_c.snd_nxt += (unsigned long)n;

    while (!br_c.finished && (long)(br_now_us() - t0) < (long)ms * 1000L) {
        if (br_c.reset) break;
        br_wait_us(10000);
    }
    br_c.state = 0;
    br_page[br_page_len] = 0;
    return br_page_len;
}

/* ===== HTML → 文字 ====================================================== */
/* タグを落とし、script/style の中身を捨て、空白を畳む。実体参照は主要なものだけ。 */
int browser_to_text(const char *src, int n, char *dst, int cap)
{
    int i = 0, o = 0, sp = 0;
    while (i < n && o < cap - 1) {
        if (src[i] == '<') {
            int skip_body = 0;
            if (i + 7 < n && b_eqn(src + i + 1, "script", 6)) skip_body = 1;
            if (i + 6 < n && b_eqn(src + i + 1, "style", 5))  skip_body = 1;
            while (i < n && src[i] != '>') i++;
            if (i < n) i++;
            if (skip_body) {                       /* 閉じタグまで捨てる */
                while (i < n) {
                    if (src[i] == '<' && i + 1 < n && src[i+1] == '/') {
                        while (i < n && src[i] != '>') i++;
                        if (i < n) i++;
                        break;
                    }
                    i++;
                }
            }
            if (!sp && o > 0) { dst[o++] = ' '; sp = 1; }
            continue;
        }
        if (src[i] == '&') {                       /* 実体参照 */
            static const struct { const char *s; char c; } ent[] = {
                {"amp;",'&'},{"lt;",'<'},{"gt;",'>'},{"quot;",'"'},{"#39;",'\''},{"nbsp;",' '},{0,0}};
            int hit = 0;
            for (int k = 0; ent[k].s; k++) {
                int L = b_len(ent[k].s);
                if (i + 1 + L <= n && b_eqn(src + i + 1, ent[k].s, L)) {
                    dst[o++] = ent[k].c; i += 1 + L; hit = 1; break; }
            }
            if (hit) { sp = 0; continue; }
        }
        { char c = src[i++];
          if (c == '\r') continue;
          if (c == '\n' || c == '\t' || c == ' ') { if (!sp && o > 0) { dst[o++] = ' '; sp = 1; } }
          else { dst[o++] = c; sp = 0; } }
    }
    dst[o] = 0;
    return o;
}

/* ===== 入口 ============================================================= */
static char br_url[256]  = "http://airilab.app/";
static char br_text[16384];
static volatile int br_text_len;
static volatile int br_status;      /* >0 受信バイト数 / <=0 失敗 */
static char br_note[128] = "not fetched yet";

const char *browser_url(void)  { return br_url; }
const char *browser_text(void) { return br_text; }
int  browser_text_len(void)    { return br_text_len; }
int  browser_status(void)      { return br_status; }
const char *browser_note(void) { return br_note; }

/* 経路まわりの見え方を返す（"ip/mask/gw/dns gwmac=..." の形）。
 * 「経路がありません」が出たとき、DHCP が何を渡しているのか、
 * ゲートウェイの MAC を引けているのかを外から確かめるための計器。 */
int browser_netinfo(char *d, int cap)
{
    unsigned char ip[4], mask[4], gw[4], dns[4], mac[6];
    net_responder_get_ip(ip); br_get_mask(mask); br_get_gw(gw); br_get_dns(dns);
    int o = 0;
    #define PB(x) do { int v=(x); if(v>=100){ if(o<cap-1) d[o++]=(char)('0'+v/100); } \
                       if(v>=10){ if(o<cap-1) d[o++]=(char)('0'+(v/10)%10); } \
                       if(o<cap-1) d[o++]=(char)('0'+v%10); } while(0)
    #define PS(s) do { const char *q=(s); while(*q && o<cap-1) d[o++]=*q++; } while(0)
    PS("ip="); PB(ip[0]);PS(".");PB(ip[1]);PS(".");PB(ip[2]);PS(".");PB(ip[3]);
    PS(" mask="); PB(mask[0]);PS(".");PB(mask[1]);PS(".");PB(mask[2]);PS(".");PB(mask[3]);
    PS(" gw="); PB(gw[0]);PS(".");PB(gw[1]);PS(".");PB(gw[2]);PS(".");PB(gw[3]);
    PS(" dns="); PB(dns[0]);PS(".");PB(dns[1]);PS(".");PB(dns[2]);PS(".");PB(dns[3]);
    PS(" gwmac=");
    if (br_arp_lookup(gw, mac)) {
        static const char hx[] = "0123456789abcdef";
        for (int i = 0; i < 6; i++) { if (i && o<cap-1) d[o++]=':';
            if (o<cap-1) d[o++]=hx[mac[i]>>4]; if (o<cap-1) d[o++]=hx[mac[i]&15]; }
    } else PS("(未解決)");
    PS(" arp=");
    { int n=0; for (int i=0;i<8;i++) if (br_arp[i].used) n++; PB(n); }
    #define PL(x) do { long v=(x); char nb[16]; int k=0; if(v==0) nb[k++]='0'; \
                       while(v>0){ nb[k++]=(char)('0'+v%10); v/=10; } while(k>0 && o<cap-1) d[o++]=nb[--k]; } while(0)
    PS(" seg="); PL(br_st_seg); PS(" ooo="); PL(br_st_ooo); PS(" finooo="); PL(br_st_finooo);
    PS(" trunc="); PL(br_st_trunc); PS(" retry="); PL(br_st_retry);
    PS(" dup="); PL(br_st_dup); PS(" fut="); PL(br_st_fut);
    { extern void rp1eth_rx_stats(unsigned int *, unsigned int *, unsigned int *);
      unsigned int fr, rre, ovr; rp1eth_rx_stats(&fr, &rre, &ovr);
      br_st_gem_frames += fr; br_st_gem_rre += rre; br_st_gem_ovr += ovr; }
    PS(" gemrx="); PL((long)br_st_gem_frames); PS(" gemrre="); PL((long)br_st_gem_rre); PS(" gemovr="); PL((long)br_st_gem_ovr);
    #undef PL
    #undef PB
    #undef PS
    d[o] = 0; return o;
}
int  browser_raw_len(void)     { return br_page_len; }
const char *browser_raw(void)  { return br_page; }

/* ===== 表示する文書 =====================================================
 * 取得に成功した HTML だけをここに置き、整形（system/html.c）して窓に出す。
 * 取得の途中経過（辞書・日本語版・切れた本文）は br_page に留まり、ここへは来ない
 * ＝ 更新に失敗しても画面は前のまま。 */
extern void html_layout(const char *h, int n, int width_px);
extern int  html_height(void);
extern void html_draw(int x0, int y0, int w, int h, int scroll_y, unsigned int bg);
extern int  html_text(char *dst, int cap);
extern int  html_i18n_apply(const char *h, int n, const char *dict, int dl, char *out, int cap);
extern void html_set_css(const char *css, int n);
extern int  html_find_stylesheet(const char *h, int n, char *out, int cap);
extern int  html_link_at(int x, int y, const char **href);
static char br_doc[65536];
static int  br_doc_len;
static const char *br_body; static int br_body_len;   /* 直近の取得の本文（ヘッダ抜き）。fetch1 成功時のみ有効 */
static int  br_layout_w = 674;         /* 直近に整形した幅（窓の幅が変われば描画側で組み直す） */
static int  br_scroll_px, br_view_h = 400;
static void br_present(const char *html, int n)
{
    if (n > (int)sizeof br_doc - 1) n = (int)sizeof br_doc - 1;
    if (html != br_doc) for (int i = 0; i < n; i++) br_doc[i] = html[i];
    br_doc[n] = 0; br_doc_len = n;
    html_layout(br_doc, br_doc_len, br_layout_w);
    br_text_len = html_text(br_text, sizeof br_text);
    br_scroll_px = 0;
}

/* ===== ページの控えと言語 ================================================
 * 取得した HTML（日本語のまま）を br_page_html に置く。表示は言語に応じて
 *   ja: そのまま整形（日本語フォントで描く）
 *   en: data-i18n の中身を辞書で差し替えた HTML を整形
 * 切替はネットワークを使わない（控えから組み直すだけ）。 */
static char br_page_html[65536];
static int  br_page_html_len;
static char br_cur_url[256] = "http://airilab.app/";   /* いま表示しているページ */
static int  br_lang = 0;                                /* 0=en 1=ja */
static char br_css_url[256];                            /* 読み込み済みの外部 CSS の URL（同じなら取り直さない） */
#define BR_DICTCAP 49152
static char br_dict[BR_DICTCAP];
static int  br_dict_len;
int  browser_lang(void) { return br_lang; }

/* 相対 URL を base（http://host/path）の上で解く */
static void br_resolve_url(const char *base, const char *href, char *out, int cap)
{
    int o = 0;
    if (b_eqn(href, "http://", 7) || b_eqn(href, "https://", 8)) { b_cpy(out, href, cap); return; }
    /* base の host 部分 */
    const char *p = base; if (b_eqn(p, "http://", 7)) p += 7;
    const char *hs = p; while (*p && *p != '/') p++;
    const char *pre = "http://"; for (int i = 0; pre[i] && o < cap - 1; i++) out[o++] = pre[i];
    for (const char *q = hs; q < p && o < cap - 1; q++) out[o++] = *q;
    if (href[0] == '/') { if (href[1] == '/') { b_cpy(out, "http:", cap); int k = 5; for (int i = 0; href[i] && k < cap - 1; i++) out[k++] = href[i]; out[k] = 0; return; }
                          for (int i = 0; href[i] && o < cap - 1; i++) out[o++] = href[i]; out[o] = 0; return; }
    /* 相対：base の最後の / まで */
    const char *last = p; for (const char *q = p; *q; q++) if (*q == '/') last = q;
    for (const char *q = p; q <= last && o < cap - 1; q++) out[o++] = *q;
    if (o == 0 || out[o-1] != '/') { if (o < cap - 1) out[o++] = '/'; }
    for (int i = 0; href[i] && o < cap - 1; i++) out[o++] = href[i];
    out[o] = 0;
}

static void br_present_lang(void)
{
    if (br_page_html_len <= 0) return;
    if (br_lang == 1 || br_dict_len <= 0) { br_present(br_page_html, br_page_html_len); return; }
    static char en[65536];
    int el = html_i18n_apply(br_page_html, br_page_html_len, br_dict, br_dict_len, en, sizeof en);
    if (el <= 0) { br_present(br_page_html, br_page_html_len); return; }
    br_present(en, el);
}
void browser_set_lang(int ja)
{
    br_lang = ja ? 1 : 0;
    br_present_lang();
    b_cpy(br_note, br_lang ? "ok (ja)" : "ok (en)", sizeof br_note);
}

/* URL を分解して取りに行く。戻り値は受信バイト数（<=0 は失敗）。 */
static char          br_dns_cache_name[128];
static unsigned char br_dns_cache_ip[4];

/* ヘッダ部から Content-Length を読む。無ければ -1。 */
static long br_content_length(const char *h, int n)
{
    const char *k = "content-length:"; int kl = b_len(k);
    for (int i = 0; i + kl < n; i++) {
        if (!b_eqn(h + i, k, kl)) continue;
        int p = i + kl; while (p < n && h[p] == ' ') p++;
        long v = 0; int d = 0;
        while (p < n && h[p] >= '0' && h[p] <= '9') { v = v * 10 + (h[p] - '0'); p++; d = 1; }
        return d ? v : -1;
    }
    return -1;
}

static int browser_fetch1(const char *url);
static int browser_fetch_raw(const char *url)      /* 取るだけ（表示しない） */
{
    int r = browser_fetch1(url);
    if (r == -5 || r == -3) { br_st_retry++; r = browser_fetch1(url); }   /* 切れた／繋がらない → 一度だけやり直す */
    return r;
}
int browser_fetch(const char *url)                 /* 取って、そのまま表示する */
{
    int r = browser_fetch_raw(url);
    if (r > 0) br_present(br_body, br_body_len);
    return r;
}
static int browser_fetch1(const char *url)
{
    char host[128], path[192];
    const char *p = url;
    if (b_eqn(p, "http://", 7)) p += 7;
    else if (b_eqn(p, "https://", 8)) { b_cpy(br_note, "https は未対応（平文 http で開く）", sizeof br_note);
                                        br_status = -10; return -10; }
    { int i = 0; while (p[i] && p[i] != '/' && i < 127) { host[i] = p[i]; i++; } host[i] = 0; p += i; }
    if (*p == 0) b_cpy(path, "/", sizeof path); else b_cpy(path, p, sizeof path);
    b_cpy(br_url, url, sizeof br_url);
    br_port = 80;
    { char *c = host; while (*c && *c != ':') c++;        /* host:port */
      if (*c == ':') { *c = 0; c++; int v = 0; while (*c >= '0' && *c <= '9') v = v * 10 + (*c++ - '0'); if (v > 0 && v < 65536) br_port = v; } }

    unsigned char ip[4];
    { int d = 0, v = 0, k = 0, ok = 1;             /* 数字ならそのまま IP として読む */
      for (int i = 0; host[i]; i++) {
        if (host[i] >= '0' && host[i] <= '9') { v = v * 10 + (host[i] - '0'); d = 1; }
        else if (host[i] == '.') { if (!d || k > 3) { ok = 0; break; } ip[k++] = (unsigned char)v; v = 0; d = 0; }
        else { ok = 0; break; } }
      if (ok && d && k == 3) { ip[3] = (unsigned char)v; }
      else if (br_dns_query(host, ip)) {
        /* 引けた名前は覚える。実機では 60 秒ごとの更新のうち数回に一度
           DNS が応答せず（4 回再送しても）、そのたびに窓が失敗表示に落ちていた。 */
        b_cpy(br_dns_cache_name, host, sizeof br_dns_cache_name);
        for (int i = 0; i < 4; i++) br_dns_cache_ip[i] = ip[i]; }
      else if (br_dns_cache_name[0] && b_eqn(host, br_dns_cache_name, b_len(host) + 1)) {
        for (int i = 0; i < 4; i++) ip[i] = br_dns_cache_ip[i];   /* 前回の答えで進む */
      } else {
        b_cpy(br_note, "名前を引けませんでした（DNS 応答なし）", sizeof br_note);
        br_status = -4; return -4; } }

    int r = br_http_get(ip, host, path, 15000);   /* 再送の待ち（RTO は倍々に伸びる）を吸えるだけ取る */
    if (r <= 0) {
        if (r == -1) b_cpy(br_note, "経路がありません（ゲートウェイの MAC を引けず）", sizeof br_note);
        else if (r == -2) b_cpy(br_note, "接続を拒否されました（RST）", sizeof br_note);
        else if (r == -3) b_cpy(br_note, "接続できません（SYN に応答なし）", sizeof br_note);
        else b_cpy(br_note, "本文が空です", sizeof br_note);
        br_status = r; return r;
    }
    { const char *body = br_page; int bl = br_page_len;   /* ヘッダを飛ばす */
      for (int i = 0; i + 3 < br_page_len; i++)
        if (br_page[i]=='\r'&&br_page[i+1]=='\n'&&br_page[i+2]=='\r'&&br_page[i+3]=='\n') {
          body = br_page + i + 4; bl = br_page_len - (i + 4); break; }
      /* Content-Length と突き合わせる。短ければ途中で切れている（段の欠け）ので失敗にする。
         切れた本文を「ok」で出すと、欠けたことが画面からは分からない。 */
      { long want = br_content_length(br_page, (int)(body - br_page));
        if (want >= 0 && (long)bl < want) {
            b_cpy(br_note, "本文が途中で切れました", sizeof br_note);
            { int o = b_len(br_note); char nb[32]; int k = 0; long v = bl;
              const char *lead = " ("; for (int i=0; lead[i]; i++) br_note[o++] = lead[i];
              if (v == 0) nb[k++] = '0'; while (v > 0) { nb[k++] = (char)('0' + v % 10); v /= 10; }
              while (k > 0) br_note[o++] = nb[--k];
              br_note[o++] = '/'; v = want; k = 0;
              while (v > 0) { nb[k++] = (char)('0' + v % 10); v /= 10; }
              while (k > 0) br_note[o++] = nb[--k];
              br_note[o++] = ')'; br_note[o] = 0; }
            br_st_trunc++;
            br_status = -5; return -5; } }
      br_body = body; br_body_len = bl; }
    b_cpy(br_note, "ok", sizeof br_note);
    br_status = r;
    return r;
}


/* 起動時・定期更新で読む「ホーム」。
 * 日本語のまま出すと機内フォントに字形が無く、窓が '.' の列で埋まる
 * （実機の画面で確認した）。英語版を組み立てて出す ―― airilab.app の英語は
 * HTML に無く /js/i18n.js の辞書にあるので browser_fetch_en で差し替える。
 * 辞書が取れなかったときだけ素の取得に落とす（読めないが「届いている」ことは分かる）。 */
static const char *br_home      = "http://airilab.app/";
static const char *br_home_dict = "http://airilab.app/js/i18n.js";
int browser_fetch_en(const char *page_url, const char *dict_url);
static int  br_have_en = 0;           /* 英語版を一度でも出せたか */
int browser_parse_ip(const char *s, unsigned char *out);
static int br_wifi_has_url(const char *url)           /* URL のホストが WiFi の副網の IP か */
{
    const char *p = url; if (b_eqn(p, "http://", 7)) p += 7;
    char h[64]; int i = 0; while (p[i] && p[i] != '/' && i < 63) { h[i] = p[i]; i++; } h[i] = 0;
    unsigned char ip[4];
    return browser_parse_ip(h, ip) && br_wifi_has(ip);
}
static int br_fetch_home(void)
{
    const char *u = br_have_en ? br_cur_url : br_home;   /* 辿った先にいるなら、そのページを更新する */
    int r = browser_fetch_en(u, br_home_dict);
    if (r <= 0 && !br_have_en) r = browser_fetch_en(u, br_home_dict);   /* 起動直後は一度で通らないことがある */
    if (r > 0) { br_have_en = 1; return r; }
    if (!br_have_en) return browser_fetch(br_home);   /* まだ何も出せていない：素の本文でも出す */
    /* 更新に失敗。表示は br_present を通っていないので前のまま。理由だけ note に残す（窓の 2 行目に出る）。 */
    { static char why[128]; b_cpy(why, br_note, sizeof why);
      b_cpy(br_url, br_cur_url, sizeof br_url);
      b_cpy(br_note, br_lang ? "ok (ja) / update failed: " : "ok (en) / update failed: ", sizeof br_note);
      { int o = b_len(br_note); for (int i = 0; why[i] && o < (int)sizeof(br_note)-1; i++) br_note[o++] = why[i]; br_note[o] = 0; } }
    br_status = br_text_len;
    return br_text_len;
}

/* 起動時に一度読む。ネットワークが立ち上がるのを少し待ってから。 */
void browser_boot(void)
{
    uart_puts("browser: fetching ");
    uart_puts(br_home);
    uart_puts(" (en) ...\n");
    int r = br_fetch_home();
    uart_puts("browser: ");
    (void)r; uart_puts(br_note);          /* "ok (en)" か失敗の理由 */
    uart_puts("\n");
}

/* ===== 画面に出す ======================================================
 * 窓の中に本文を折り返して描く。ASCII 以外（UTF-8 の日本語など）は
 * 機内フォントに字形が無いので '.' に落とす ―― 化けた点を並べるより、
 * 「ここに読めない字がある」と分かる形にしておく。
 * スクロールは frame 番号ではなく明示的な行送り（browser_scroll）で行う。 */
extern void draw_string_at(int x, int y, const char *s, unsigned int fg, unsigned int bg);
extern void fill_rect(int x, int y, int w, int h, unsigned int c);

#define WM_TITLEBAR_H_LOCAL 22
void browser_scroll(int d)           /* d は px。可視域に合わせて丸める */
{
    br_scroll_px += d;
    int maxs = html_height() - br_view_h; if (maxs < 0) maxs = 0;
    if (br_scroll_px > maxs) br_scroll_px = maxs;
    if (br_scroll_px < 0) br_scroll_px = 0;
}
/* 窓の中を左クリック：
 *   リンクの上 → そのページへ（取得は wm の巡回で行う。クリックの経路で通信しない）
 *   [EN] ボタン → 言語切替（控えから組み直すだけ）
 *   それ以外  → 上半分で一画面戻り、下半分で一画面進む（キー入力は Shell が取るため） */
static char br_pending_url[256];          /* 巡回で取りに行く URL（空なら無し） */
static int  br_pending_lang = -1;         /* 巡回で切り替える言語（-1 なら無し） */
void browser_click(void *selfv, int lx, int ly)
{
    struct wshape { int x, y, width, height; };
    struct wshape *w = (struct wshape *)selfv;
    int top = 22 + 6 + 26;                                     /* 題名帯＋URL/注記の 2 行 */
    if (ly >= 22 && ly < top) { b_cpy(br_pending_url, "xinu://mesh", sizeof br_pending_url); return; }   /* URL 行をクリック → メッシュ一覧 */
    if (ly >= top && lx >= 8 && lx < w->width - 12) {
        const char *href = 0;
        int k = html_link_at(lx - 8, ly - top + br_scroll_px, &href);
        if (k == 2) { br_pending_lang = !br_lang; return; }
        if (k == 1 && href) {
            if (href[0] == '#') { br_scroll_px = 0; return; }   /* ページ内リンクは先頭へ */
            br_resolve_url(br_cur_url, href, br_pending_url, sizeof br_pending_url);
            b_cpy(br_note, "loading...", sizeof br_note);
            return;
        }
    }
    int mid = top + (w->height - top) / 2;
    int page = br_view_h - 24; if (page < 40) page = 40;
    browser_scroll(ly < mid ? -page : page);
}

void browser_draw_window(void *selfv, unsigned int frame)
{
    (void)frame;
    /* window_t の先頭 4 つ（x,y,width,height）だけ使う。wm.h を取り込まずに
       済ませるため、必要な分だけを int の並びとして読む。 */
    struct wshape { int x, y, width, height; };
    struct wshape *w = (struct wshape *)selfv;
    unsigned int bg = 0xFF0A0E14U, fg = 0xFFE8EEF8U, hi = 0xFF80D0FFU, dim = 0xFF8090A8U;

    int xb = w->x + 8;
    int yb = w->y + WM_TITLEBAR_H_LOCAL + 6;
    int cw = w->width - 26;                 /* 右端の帯（スクロール位置）の分を空ける */
    int cols = cw / 8;  if (cols < 20) cols = 20;  if (cols > 200) cols = 200;
    int rows = (w->height - WM_TITLEBAR_H_LOCAL - 16) / 10;  if (rows < 2) rows = 2;

    fill_rect(w->x + 1, w->y + WM_TITLEBAR_H_LOCAL + 1,
              w->width - 2, w->height - WM_TITLEBAR_H_LOCAL - 2, bg);

    draw_string_at(xb, yb, br_url, hi, bg);
    { char st[96]; int o = 0;
      const char *n = br_note;
      const char *lead = "  ";
      for (int i = 0; lead[i] && o < 90; i++) st[o++] = lead[i];
      for (int i = 0; n[i] && o < 90; i++) st[o++] = (unsigned char)n[i] < 0x80 ? n[i] : '.';
      st[o] = 0;
      draw_string_at(xb, yb + 10, st, dim, bg); }

    /* 本文は system/html.c の整形結果を描く。窓の幅が変わっていれば組み直す。 */
    (void)cols; (void)rows; (void)fg;
    { int vh = w->height - WM_TITLEBAR_H_LOCAL - 16 - 26;
      if (vh < 16) vh = 16;
      br_view_h = vh;
      if (br_layout_w != cw && br_doc_len > 0) { br_layout_w = cw; html_layout(br_doc, br_doc_len, cw); }
      if (br_doc_len > 0) html_draw(xb, yb + 26, cw, vh, br_scroll_px, bg);
      else draw_string_at(xb, yb + 26, "(no page loaded yet)", dim, bg);
      /* 右端に位置の目安（縦の帯） */
      { int th = html_height(); if (th > vh && th > 0) {
          int bh = vh * vh / th; if (bh < 8) bh = 8;
          int by = yb + 26 + (vh - bh) * br_scroll_px / (th - vh > 0 ? th - vh : 1);
          fill_rect(w->x + w->width - 6, yb + 26, 3, vh, 0xFF1A2230U);
          fill_rect(w->x + w->width - 6, by, 3, bh, 0xFF60FFC0U); } } }
}

/* ===== wm の巡回から駆動する ============================================
 * ★ 専用プロセスでは走らない。HDMI がある構成では kernel_main が wm_run() に
 *   入り、そこは NULLPROC（idle）として回る。proc_preempt は
 *   `if (!IS_IDLE(currpid))` で idle を除外するので、**他のプロセスへ切り替わらない**。
 *   ネットワークが動くのは受信の巡回が wm のループから直接呼ばれているからで、
 *   スケジューラが回っているからではない。実験1で見つけた
 *   「核0 は idle だから何もしない」と同じ構造である。
 *   したがって basicwin_poll_pending / wifi_adhoc_poll_pending と同じく、
 *   wm の巡回から呼ぶ。取得中は画面が数秒止まるが、それらと同じ割り切りである。 */
void browser_poll_pending(void)
{
    static long ticks = 0;
    static int  first_done = 0;
    ticks++;
    if (!first_done) {
        if (ticks < 200) return;              /* 約10秒。リンク確立を待つ */
        ticks = 0;
        browser_boot();
        if (br_status > 0) first_done = 1;    /* 通ったら以後は定期更新へ */
        return;                                /* 失敗したらまた 10 秒後に試す */
    }
    if (br_pending_lang >= 0) { browser_set_lang(br_pending_lang); br_pending_lang = -1; }
    if (br_pending_url[0]) {                                    /* クリックで辿る */
        static char u[256]; b_cpy(u, br_pending_url, sizeof u); br_pending_url[0] = 0;
        if (b_eqn(u, "https://", 8)) { b_cpy(br_note, "https は未対応（このリンクは開けません）", sizeof br_note); return; }
        int r = browser_fetch_en(u, br_home_dict);
        if (r <= 0) { static char why[128]; b_cpy(why, br_note, sizeof why);
                      b_cpy(br_note, "open failed: ", sizeof br_note);
                      int o = b_len(br_note); for (int i = 0; why[i] && o < (int)sizeof(br_note)-1; i++) br_note[o++] = why[i]; br_note[o] = 0; }
        ticks = 0; return;
    }
    if (ticks >= 1200) { ticks = 0; br_fetch_home(); }         /* 約60秒ごと */
}

/* ===== このサイトの英語版を出す =========================================
 * airilab.app の英語は **HTML に入っていない**。サーバが返すのは日本語版だけで、
 * `/js/i18n.js` の DICT.en を JavaScript が data-i18n="key" の要素へ流し込む。
 * 板に JS エンジンは無いので、同じ置換をこちらで行う ――
 * HTML に現れる data-i18n の **順に** 辞書の英語値を並べる。
 * DOM を作らずに済むのは、このサイトの可視文字列がほぼ全部 data-i18n 要素の
 * 中にあるためで、**汎用の JS 実行ではなくこの方式に的を絞った実装**である。 */
/* （辞書の引き方は system/html.c の dict_lookup に移した） */

/* ページを取り、外部 CSS（<link rel=stylesheet>）も取り、控えて、いまの言語で表示する。
 * 辞書（i18n.js）は一度取れば使い回す。戻り値は本文の文字数（<=0 は失敗）。 */
/* 組み込みページ xinu://mesh ―― メッシュ（無線）で見えている板を一覧し、各板の
 * ページ（GET / = 各 Xinu の HTTP 玄関）へのリンクを並べる。通信せずに作る。 */
extern int wifi_mesh_peers(unsigned char *out, int cap);
extern int wifi_mesh_self(void);
static int br_builtin_mesh(char *out, int cap)
{
    int o = 0;
    #define PUT(s) do { const char *q_ = (s); while (*q_ && o < cap - 1) out[o++] = *q_++; } while (0)
    #define PUTN(v) do { int v_ = (v); char nb_[12]; int k_ = 0; if (v_ == 0) nb_[k_++] = '0'; while (v_ > 0 && k_ < 11) { nb_[k_++] = (char)('0' + v_ % 10); v_ /= 10; } while (k_ > 0 && o < cap - 1) out[o++] = nb_[--k_]; } while (0)
    PUT("<html><body><h1>Xinu mesh</h1>");
    if (!wifi_connected()) {
        PUT("<p>WiFi (IBSS) is not joined. Run <code>wifi adhoc &lt;ssid&gt; &lt;ch&gt; &lt;node&gt;</code> or <code>/wifi-adhoc</code> first.</p>");
    } else {
        unsigned char ip[4]; wifi_ipaddr(ip);
        PUT("<p>This board: node "); PUTN(wifi_mesh_self()); PUT(" ("); PUTN(ip[0]); PUT("."); PUTN(ip[1]); PUT("."); PUTN(ip[2]); PUT("."); PUTN(ip[3]); PUT(")</p>");
        unsigned char peers[32]; int n = wifi_mesh_peers(peers, 32);
        if (n == 0) PUT("<p>No neighbours heard yet (HELLO every 2 s).</p>");
        else {
            PUT("<h2>Neighbours</h2><ul>");
            for (int i = 0; i < n; i++) {
                PUT("<li><a href=\"http://"); PUTN(ip[0]); PUT("."); PUTN(ip[1]); PUT("."); PUTN(ip[2]); PUT("."); PUTN(peers[i]);
                PUT("/\">node "); PUTN(peers[i]); PUT(" - http://"); PUTN(ip[0]); PUT("."); PUTN(ip[1]); PUT("."); PUTN(ip[2]); PUT("."); PUTN(peers[i]); PUT("/</a></li>");
            }
            PUT("</ul>");
        }
    }
    PUT("<hr><p><a href=\"http://airilab.app/\">Home: airilab.app</a></p></body></html>");
    #undef PUT
    #undef PUTN
    out[o] = 0; return o;
}

int browser_fetch_en(const char *page_url, const char *dict_url)
{
    if (b_eqn(page_url, "xinu://", 7)) {              /* 組み込みページ（通信しない） */
        br_page_html_len = br_builtin_mesh(br_page_html, sizeof br_page_html);
        b_cpy(br_cur_url, page_url, sizeof br_cur_url);
        html_set_css("", 0); br_css_url[0] = 0;
        br_present(br_page_html, br_page_html_len);
        b_cpy(br_url, page_url, sizeof br_url);
        b_cpy(br_note, "ok (mesh)", sizeof br_note);
        br_status = br_text_len; return br_text_len;
    }
    if (br_dict_len <= 0 && !br_wifi_has_url(page_url)) {   /* 辞書は初回だけ（表示はしない）。メッシュの板には要らない */
        int r = browser_fetch_raw(dict_url);
        if (r <= 0) { b_cpy(br_note, "i18n.js を取得できません", sizeof br_note); return -20; }
        br_dict_len = br_body_len < BR_DICTCAP-1 ? br_body_len : BR_DICTCAP-1;
        for (int i = 0; i < br_dict_len; i++) br_dict[i] = br_body[i];
        br_dict[br_dict_len] = 0;
    }
    int r = browser_fetch_raw(page_url);             /* 本体 */
    if (r <= 0) return r;                            /* note は fetch が書いている */
    { /* text/plain（Xinu 板の GET / など）は <pre> で包んで行を保つ */
      int plain = 0;
      { const char *k = "content-type:"; int kl = b_len(k); int hl = (int)(br_body - br_page);
        for (int i = 0; i + kl + 10 < hl; i++)
            if (b_eqn(br_page + i, k, kl)) { int q = i + kl; while (q < hl && br_page[q] == ' ') q++;
                                              plain = b_eqn(br_page + q, "text/plain", 10); break; } }
      int o = 0, cap = (int)sizeof br_page_html - 1;
      if (plain) { const char *w = "<html><body><pre>"; for (int i = 0; w[i] && o < cap; i++) br_page_html[o++] = w[i]; }
      for (int i = 0; i < br_body_len && o < cap; i++) {
          char c = br_body[i];
          if (plain && c == '<') { if (o + 4 <= cap) { br_page_html[o++]='&'; br_page_html[o++]='l'; br_page_html[o++]='t'; br_page_html[o++]=';'; } continue; }
          if (plain && c == '&') { if (o + 5 <= cap) { br_page_html[o++]='&'; br_page_html[o++]='a'; br_page_html[o++]='m'; br_page_html[o++]='p'; br_page_html[o++]=';'; } continue; }
          br_page_html[o++] = c;
      }
      if (plain) { const char *w = "</pre></body></html>"; for (int i = 0; w[i] && o < cap; i++) br_page_html[o++] = w[i]; }
      br_page_html[o] = 0; br_page_html_len = o; }
    b_cpy(br_cur_url, page_url, sizeof br_cur_url);
    /* 外部 CSS。URL が前回と同じなら取り直さない。取れなくても本文は出す（CSS 無しで） */
    { static char href[256], cssurl[256];
      if (html_find_stylesheet(br_page_html, br_page_html_len, href, sizeof href)) {
          br_resolve_url(page_url, href, cssurl, sizeof cssurl);
          if (!b_eqn(cssurl, br_css_url, b_len(cssurl) + 1)) {
              int c = browser_fetch_raw(cssurl);
              if (c > 0) { html_set_css(br_body, br_body_len); b_cpy(br_css_url, cssurl, sizeof br_css_url); }
              else html_set_css("", 0);
          }
      } else { html_set_css("", 0); br_css_url[0] = 0; } }
    br_present_lang();
    b_cpy(br_url, page_url, sizeof br_url);
    b_cpy(br_note, br_lang ? "ok (ja)" : "ok (en)", sizeof br_note);
    br_status = br_text_len;
    return br_text_len;
}

/* ===== JS を本当に実行して組み立てる ====================================
 * これまでの ?en=1 は、このサイトの i18n 方式に的を絞った置換だった。
 * こちらは **機内 JS 処理系で /js/i18n.js を実行**し、その副作用（innerHTML への
 * 代入）を DOM 側が記録し、差し替え後の HTML を本文にする。
 * つまり「サイトの JS が組み立てた画面」を板の上で再現する。 */
extern void js_reset(void);
extern int  js_run(const char *src, int len);
extern int  js_failed(void);
extern const char *js_error(void);
extern void js_dom_load(const char *html, int len);
extern int  js_dom_count(void);
extern int  js_dom_render(char *dst, int cap);

static char br_html[BR_RXCAP];   /* 取得した HTML の控え（辞書取得で br_page が潰れる） */
static int  br_html_len;

int browser_fetch_js(const char *page_url, const char *js_url)
{
    int r = browser_fetch(page_url);                 /* まず本体を取る */
    if (r <= 0) return r;
    { const char *b = br_page; int bl = br_page_len;
      for (int i = 0; i + 3 < br_page_len; i++)
        if (br_page[i]=='\r'&&br_page[i+1]=='\n'&&br_page[i+2]=='\r'&&br_page[i+3]=='\n') {
          b = br_page + i + 4; bl = br_page_len - (i + 4); break; }
      br_html_len = bl < BR_RXCAP-1 ? bl : BR_RXCAP-1;
      for (int i = 0; i < br_html_len; i++) br_html[i] = b[i];
      br_html[br_html_len] = 0; }

    r = browser_fetch(js_url);                       /* 次に JS を取る */
    if (r <= 0) { b_cpy(br_note, "JS を取得できません", sizeof br_note); br_status = -30; return -30; }
    const char *jb = br_page; int jl = br_page_len;
    for (int i = 0; i + 3 < br_page_len; i++)
        if (br_page[i]=='\r'&&br_page[i+1]=='\n'&&br_page[i+2]=='\r'&&br_page[i+3]=='\n') {
            jb = br_page + i + 4; jl = br_page_len - (i + 4); break; }

    js_reset();
    js_dom_load(br_html, br_html_len);               /* HTML から要素を拾う */
    js_run(jb, jl);                                  /* 実行 ―― 副作用が DOM に入る */
    if (js_failed()) { extern int js_error_pos(void), js_error_snippet(char *, int);
                       static char sn[128]; js_error_snippet(sn, sizeof sn);
                       b_cpy(br_note, "JS: ", sizeof br_note);
                       { int o=4; const char *e=js_error();
                         while (*e && o < (int)sizeof(br_note)-1) br_note[o++]=*e++; br_note[o]=0; }
                       { int o2=b_len(br_note); const char *tag=" near: ";
                         for(int i=0;tag[i]&&o2<(int)sizeof(br_note)-1;i++) br_note[o2++]=tag[i];
                         for(int i=0;sn[i]&&o2<(int)sizeof(br_note)-1;i++) br_note[o2++]=sn[i];
                         br_note[o2]=0; }
                       br_status = -31; return -31; }

    { static char outbuf[BR_RXCAP];
      int n = js_dom_render(outbuf, sizeof outbuf);
      br_text_len = browser_to_text(outbuf, n, br_text, sizeof br_text); }
    b_cpy(br_url, page_url, sizeof br_url);
    b_cpy(br_note, "ok (js)", sizeof br_note);
    br_status = br_text_len;
    return br_text_len;
}
int browser_dom_count(void) { return js_dom_count(); }
