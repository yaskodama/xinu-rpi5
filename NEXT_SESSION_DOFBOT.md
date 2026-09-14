# DOFBOT（Xinu＋AIPL）—— 次のセッションへの引き継ぎ

最終更新: 2026-09-14 15:50（セッション終了時）

## 30 秒で状況を掴む

| 板 | 何が動いているか | 番地 |
|---|---|---|
| DOFBOT の Pi 5 | **Xinu**（USB メモリ `XINU5` から起動、Linux の SD は抜いてある）。腕（I²C 0x15）・USB カメラ（UVC 等時）・ファン・AIPL アクター `dofbot` | 192.168.3.101（有線） |
| もう一つの Pi 5 | Linux（Yahboom の SD）。カメラは外してある | 192.168.3.19（`yahboom.local`） |
| Mac | AIPL 正典（OCaml）、Xinu シミュレータ aice-avm（:8080） | — |

**板に焼いてあるのは build `Sep 14 2026 12:18:34`（xinu-rpi5 `3b37708`）。**
**未焼きの最新は `d4eac11`（build 15:49:58）** —— 起動時のアクター載せ＋カメラ開始を専用プロセスに移した版。
12:18 の板は再起動のたびに **HTTP が固まる可能性がある**（12:18 版で 1 回発生。UDP/9010 と ping は生きる）。
→ **次回はまず `d4eac11` を焼く**（`make pi5` → `XINU5` に `kernel_2712.img` を写す → md5 → 電源入れ直し）。

## 使い方（いつもの手順）

```sh
# 版の確認（焼いたあと必ず。HTTP が応答する≠再起動済み）
curl -s http://192.168.3.101/version | head -1
# 腕（文字列命令。シェルなら `arm ...`）
curl "http://192.168.3.101/arm/read"
curl "http://192.168.3.101/arm/pose/90/90/90/90/90/30/2000"
# カメラ（起動 50 s 後に自動開始。止まっていれば /cam/start）
curl "http://192.168.3.101/cam?w=160"          # "DW DH SRCW SRCH LEN streaming|idle"
curl "http://192.168.3.101/cam/start?frame=3&fps=10"
# ファン（温度追従が既定。手動は level=、戻すのは auto=1）
curl "http://192.168.3.101/fan"
# アクター（起動時に自動で載る。無ければ）
curl --data-binary @$HOME/dofbot_pi5/aipl/dofbot_arm.aipl http://192.168.3.101/cc
curl "http://192.168.3.101/api/x/"
```

シミュレータ: `cd ~/projects/aice-avm && ./_build/default/server.exe 8080`（`eval "$(opam env)"` 必要）→ `http://localhost:8080/#arm-xinu`
（DOFBOT arm 窓＋AIPL program 窓、映像元は Xinu のカメラ）。

振り付け（模型 → 実機）:
```sh
cd ~/dofbot_pi5/aipl && printf 'load %s\ncompile\n' "$PWD/complex.aipl" > r.repl
cd ~/aios/abclcp && ./src/abclrepl_thread -q -f ~/dofbot_pi5/aipl/r.repl
```
実測: 44 応答すべて ok（12:04 版以降、I²C 待ち 100 ms・再試行 3 回で時間切れは消えた）。

## 今日できたこと（時系列）

1. Linux 上で SSH→Python→I²C の三層を整理（レポート第 1〜3 節）。把持は未達（カメラに指が映らない）。
2. Linux を外し Xinu へ: `device/i2c/rp1i2c.c`（RP1 i2c1）＋ `system/arm.c`（電文）＋ `/arm/...`。
3. AIPL 化: 正典に `remote_call`／`arm_cmd`／実行位置通知（UDP/9011）。板に `Arm` アクター（`arm_cmd` 組込み）。
4. シミュレータに DOFBOT arm 窓（3D 模型・カメラ・スライダ・模型/実機切替）と AIPL program 窓（実行行を反転）。
5. **UVC カメラドライバ**（xHCI 等時 IN、TRB 環 512、UVC ヘッダでフレーム組立）→ 320×240 @10fps。`/cam?w=&off=` で RGB565 分割配信。
6. ファン（RP1 PWM1 ch3、温度追従）、起動時自動開始、腕基板の自動リセット、I²C 待ち延長。
7. レポート 8 頁を kodamay.org 研究レポート 90 本目として公開: `reports/2026-09-14_dofbot_xinu_aipl_uvc.pdf`。

## 踏んだ罠（再発防止）

- **EP0 の転送環は全 USB 装置で共有**。別の装置を address するとカメラの制御転送が全部時間切れ → 配信開始のたびにカメラを address し直す。
- **remote_call の要求番号を毎回 20000 から始めると、相手が「再送」と見て前回の答えを返す** → 起動時刻からずらした。
- **pkill -f のパターンに自分の bash -c 行が当たって自殺**（Linux Pi のカメラ係） → `pkill -f "^python3 -u /home/pi/cam_service.py"`。
- **腕基板はサーボ通信中に I²C 応答が 20 ms を超える**。動作直後に読みが全滅（書きは ACK）→ 待ち 100 ms・再試行、3 回全滅で基板リセット。
- **net tick で長い仕事をすると画面ループ側で当たって HTTP が固まる** → 専用プロセス（`dofbot_boot_proc`）。
- **1 枚を分割して取るあいだに最新が入れ替わる** → off=0 で写し取る（12:18 版に入っている）。
- 起動直後 30〜60 s は機内ブラウザで HTTP が待たされる（故障ではない）。
- 板を電源から入れ直すと RAM 上のアクターは消える（自動載せ直しあり）。カメラ配信も止まる（自動開始あり）。

## 次の一手（候補）

1. `d4eac11` を焼いて、再起動 3 回で HTTP が生きることを確かめる。
2. カメラの画像を AIPL から使う（`cam_frame()`／タグ検出の組込み）。HDMI 画面にカメラ窓。
3. 把持: 腕の外から見るカメラ（Linux Pi 5 に付け替えて `cam_service.py`）か接触センサが要る。手首カメラには指が映らない。
4. 9 月 7 日レポートの 6＋1 アクタ設計（関節 6＝判断層、arm 1＝io）に分ける。

## 場所

- 板側: `~/projects/xinu-rpi5`（branch `feat/smp-symmetric`）: `device/i2c/rp1i2c.c` `system/arm.c` `device/usb/rp1usb.c`（UVC は末尾）`device/genet/rp1fan.c` `loader/main.c`（`dofbot_boot_proc`）`system/tcp_server.c`（`/arm` `/cam` `/fan` `/usb/cam-probe`）`cc/cc.c`（`v_arm_cmd`）
- 正典: `~/aios/abclcp`（branch `make-src-base`）: `src/eval_thread.ml`（`remote_call` `arm_cmd` `trace_pc`）`src/typing_env.ml`
- シミュレータ: `~/projects/aice-avm`（main）: `server.ml`（`/api/arm` 模型 `sim_cmd` UDP/9011）`www/js/xinu.js`（`openArm` `openAiplProgram`）
- AIPL と道具: `~/dofbot_pi5/`（`aipl/complex.aipl` `aipl/dofbot_arm.aipl` `cam_service.py` `arm.py` `dofbot_control_path.tex/pdf`）
- レポート原稿の写し: `~/kodamay_org_site/kodamay.org/reports-src/2026-09-14_dofbot_xinu_aipl_uvc/report.tex`
- メモリ: `project-dofbot-ultra-baremetal-arm` `project-xinu-pi5-uvc-camera` `project-dofbot-pi5-real-arm`
