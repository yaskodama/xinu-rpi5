// include/wifi.h — CYW43455 SDIO WiFi (BCM2712 SDIO2 / sdhci-brcmstb).
//
// Bring-up is driven over HTTP (no boot-time auto-init), brought up in stages
// because the SDIO host layer is new and the test loop is slow.  See
// device/wifi/wifi.c.  All entry points compile to harmless stubs unless
// WIFI_SDIO_BASE is defined (pi5 only).

#ifndef XINU_RPI5_WIFI_H
#define XINU_RPI5_WIFI_H

/* trace log (paginated over HTTP because the body is small) */
const char *wifi_trace(void);
int         wifi_trace_len(void);

/* staged bring-up.  wifi_probe() = full M0..M1; wifi_probe_stage(k) stops at
 * stage k so a wedge can be bisected across reboots:
 *   -2 = S0a (power + host CAPS), -1 = S0b (pinmux + CMD5),
 *    0 = S0c (chip-id + cores), 1 = ramscan, 2 = halt, 3 = 4KB write,
 *    4 = full fw write, 5 = CR4 release, 6 = HT clock + Fn2. */
int  wifi_probe(void);
int  wifi_probe_stage(int k);
int  wifi_probe_bulk(int kb, unsigned int hz);   /* timed CMD53 throughput     */
int  wifi_probe_winwrite(int w);                 /* bisect a bad 32 KB window  */
void wifi_set_fwload_hz(unsigned int hz);

/* pinmux (the one unknown): override the sd2 funcsel guess + dump regs. */
void wifi_set_pin_fsel(unsigned int f);
void wifi_pinmux_dump(void);

/* control + data plane (post-M1) */
int  wifi_scan_run(void);
int  wifi_join_run(const char *ssid, const char *pass);
int  wifi_dhcp(void);
int  wifi_serve(int secs);
int  wifi_ping(const unsigned char *ip, int count);
void wifi_net_poll(void);            /* per-frame pump; self-gates on have-ip  */
int  wifi_connected(void);
const char *wifi_ssid(void);
void wifi_ipaddr(unsigned char *o);
void wifi_off(void);                 /* disconnect + bring the radio down       */
int  wifi_eth_tx(const unsigned char *eth, int len);  /* TX hook for tcp_server  */

/* M14 — MANET app-layer over UDP/5000 (多賀2021 4章/6章、案B 密度実験).
 * 4エージェントの push/pull を非同期 UDP で実装。ACK が受信側の内部差分を
 * 運ぶので、発火ノードは自分の時計だけで RTT と内訳を出せる(時刻同期不要)。 */
void wifi_manet_node(int n);                       /* 自ノード id (既定 = ip 末octet) */
void wifi_manet_nbr(const unsigned char *ids, int n); /* 仮想トポロジ; n=0 で全受け入れ */
void wifi_manet_cfg(int proxy, int rr_passes, int rr_verts, int ackonly);
int  wifi_manet_fire(int fid, int timeout_ms);     /* 1試行: PUSH → ACK 回収 → @B 出力 */
int  wifi_manet_pull(int timeout_ms);              /* ④収集: PULLREQ → PULLRSP 回収 */
void wifi_manet_reset(void);
void wifi_manet_status(void);
int  wifi_manet_known(unsigned short *out, int cap);
void wifi_manet_dump(void);        /* gknown をトレースへ: 到達率の厳密判定 */
int  wifi_manet_load(int pps, int ms);  /* 背景負荷: 仮想ノード群の PUSH を pps で ms 出す */
int  wifi_manet_has(int fid);           /* 厳密到達判定: gknown が fid を含むか */

#endif /* XINU_RPI5_WIFI_H */
