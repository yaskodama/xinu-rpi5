# NEXT_SESSION — xinu-rpi5 (HEAD `d9e9b81`, 2026-05-23)

## 達成サマリー (2026-05-23)

### ✅ NET-E 完了 — Mac から `ping 192.168.3.100` 動作
- Pi 4 GENET (BCM2711) で ARP request → reply、ICMP echo → echo-reply まで完走
- RTT 約 900 ms (wm tick 20 fps 律速)、静的 IP 192.168.3.100 / MAC `d8:3a:dd:a7:fd:bf`
- 半セッションかけて発掘した GENET v5 の MAC bring-up セット:
  1. **UMAC_CMD speed bits** を PHY auto-neg 結果 (BCM54213PE MII reg `0x19` AUX_STAT bits 10:8 = HCD) に追従
  2. **EXT_RGMII_OOB_CTRL** に `EXT_RGMII_LINK | EXT_ID_MODE_DIS | EXT_RGMII_MODE_EN` を立てる
  3. **UMAC_CMD** に `CMD_PROMISC | CMD_PAD_EN | CMD_CRC_FWD` を追加
- RX descriptor `len_stat` の低 16 bit の bit 11/10/9/8 は **CRC/RXER/NO/LG エラーではなく** RX checksum status + MULT/BRDCAST フラグ。誤読すると延々と「全部 CRC エラー」と思い込む罠 — 一度ハマった
- commit `0e37d9a`

### 📦 NET-F (DHCP client) — code-staged, dispatch OFF
- `system/dhcp_client.c` で DHCPDISCOVER/OFFER/REQUEST/ACK の状態機械 + volatile/aligned(64) フレームビルダー
- `dhcp_send_discover()` で wire に正しく出ることは確認 (`rc=0`, 302 bytes)
- 起動時 DISCOVER と 5 秒周期リトライは main.c から外している
- 理由: 連続 broadcast TX で GENET TX ring が劣化し ICMP reply が止まる症状再現
- `dhcp_handle_packet()` だけは `genet_rx_tick` に残してあるので、unsolicited OFFER が来れば bind 可
- 当家 LAN の router DHCP 経路 (WiFi↔LAN bridge) で OFFER が戻ってこなかった
- commits `dfddc90`, `dc87af5`

### 📦 NET-G (TCP listener) — code-staged, dispatch OFF
- `system/tcp_server.c` で単一接続 TCP listener (LISTEN → SYN_RCVD → ESTABLISHED → close)
- SYN+ACK、greeting 送出、echo、close-on-newline、FIN+ACK まで実装
- 検証で port 23 SYN は届くこと確認済 (`tcp/dbg: 192.168.3.202:52059 -> :23 flags=0x02`)
- 但し dispatch from `rx_tick` は **OFF** にしている — 理由は次節参照
- commit `d9e9b81`

## 未解決バグ — NET-G の "ICMP responder 干渉"

**症状**: `tcp_handle_packet()` を `rx_tick` から呼ぶだけで、Mac から `nc 192.168.3.100 23` を実行すると ICMP echo reply が **以後ずっと止まる** (ping 完全停止)

**観察**:
- `tcp_handle_packet` 内の `tcp_send(SYN+ACK)` を消しても症状再現
- 状態機械全体を抜いて `return 1;` だけにすると **ping 生存** (= consume + release は無罪)
- `uart_putc('X')` 1 文字追加でテストしたら、なぜか SYN が届かず X も出ず、ping 生存 — 再現条件があいまい

**仮説**:
- `uart_puts` (→ `screen_putc` → `shellwin_record_char`) の処理時間中に GENET RX 16-slot ring が overrun → `g_rx_cons` が HW の PROD と乖離 → 以後の ICMP echo を取りこぼす
- もしくは shellwin の scroll-back 操作が何かしらの shared state を壊す

**次セッションでの bisect 候補**:
1. `screen_putc` を一時的に no-op 化 (UART + shellwin だけ残す) → ping 生存なら screen 側
2. `shellwin_record_char` を一時的に no-op 化 → ping 生存なら shellwin 側
3. RX ring を 16 → 64 slot に増やして overrun 余裕を確保 → 改善するか
4. `g_rx_cons` と HW CONS_INDEX の同期に bug がないか debug 出力で再確認

## DHCP の謎を解く別ルート

NET-F の DISCOVER は wire には出てるはず (rc=0)。だが OFFER が戻ってこない理由は未確定:
- (A) router の WiFi↔LAN bridge が DHCP broadcast を片方向しか通さない (以前 ping debug 時に判明)
- (B) router の DHCP server scope に Pi 4 ENM の MAC が無い
- (C) 自前 DHCP server を Mac 上に立てて検証 (`dnsmasq` か `dhcpd`)

Mac で `sudo tcpdump -i en0 -nn -e -v 'port 67 or port 68'` をかけて Pi 4 起動 → DISCOVER が見えれば router 側問題、見えなければ bridge 側問題、と切り分けできる。

## ファイル位置 / 起動手順

```
リポ: /Users/kodamay/projects/xinu-rpi5/
branch: main
HEAD:  d9e9b81

build:
  cd compile && make pi4

焼き:
  diskutil mount /dev/disk4s1
  cp kernel8.img /Volumes/bootfs/
  sync
  diskutil eject /Volumes/bootfs

Mac 側 ARP (Pi 4 boot 後):
  sudo arp -s 192.168.3.100 d8:3a:dd:a7:fd:bf
  ping 192.168.3.100
```

## Round 1 の他の残作業 (参考)

- **S1 preemptive timer** (GIC + Generic Timer IRQ → preemptive scheduler) — kernel コアに戻りたい場合の最有力
- **X1 AIPL hello** (Py-I or JS-O runtime を xinu-rpi5 上で hello-world)
- **XHCI-B〜H** (Pi 4 USB-A キーボード/マウス: VL805 enumeration → port reset → HID transfer)
- **N0 RP1 discover** (Pi 5 RP1 I/O hub) — Pi 5 に切り替えるなら

## 次セッション最初の 3 アクション (推奨)

1. `cd /Users/kodamay/projects/xinu-rpi5 && git pull && git log --oneline -3` で HEAD 確認
2. SD カードの `kernel8.img` の md5 を `compile/kernel8.img` と照合 (古いまま試して混乱しがち)
3. Mac で `sudo arp -s 192.168.3.100 d8:3a:dd:a7:fd:bf && ping 192.168.3.100` で **まずは ping 動作スタートラインに戻る** ことを確認 (これが今日のゴール状態)

ここから NET-G bisect か、DHCP tcpdump 検証か、別軸 (S1/XHCI) に移るかを選択。
