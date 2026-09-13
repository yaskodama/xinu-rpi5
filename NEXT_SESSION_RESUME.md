# 次回再開ハンドオフ (2026-06-29 中断時点)

ユーザがXinu実機の電源を一時停止して中断。再開時にこの文書 + メモリ
`project_xinu_rpi5_multicore_actors.md` を参照すれば順調に続けられる。

## 1. 現在の確定状態

### rpi5 (192.168.3.101 / mesh 10.0.0.3)
- **稼働カーネル = `7cbf0f3f`**(SD焼済・電源OFF中)。中身:
  - ✅ **I-cache有効化**(`system/mmu.c`, SCTLR.I=1, D-cacheは安全のためOFF維持)= **1-core 3490ms→67ms の約53倍高速化**。rpi5がrpi4比 22倍遅い異常を解消し 2.4倍速へ逆転
  - ✅ **メッシュ参加高速化**(`device/wifi/wifi.c`, IBSS poll 24×500ms=12s → 40×150ms=6s、SET_SSID後待ち 400ms→100ms)= /wifi-adhoc 即connected
  - ✅ **blender表示移植**(`device/video/avm.c` = rpi4から移植、AVM2バイナリメッシュ/tri/line/mesh3d)
  - 🟡 **マルチコア・アクタースケジューラ**(`device/video/avm.c`) = **par既定OFFで休眠=安全**。`/avm-par?on=1` で実行時ON、診断カウンタ `nbatch`/`lastbn`(`system/tcp_server.c`)
- 焼き方: SD `/Volumes/bootfs` に **`compile/kernel_2712.img` のみ `cp`**(★`make install_pi5`禁止=config.txt上書き)。config.txt(1280x720/DEBUG UART/pciex4_reset=0)温存必須
- ★再起動でIBSS BSSID変わる→rpi4/rpi3は再参加要(MCCのAuto自動参加が担当)

### rpi4 (192.168.3.100), rpi3 (192.168.3.50:8080)
- 今セッションでは未改変。電源OFF中。再開時メッシュ再形成要。

### Mac sim = aice-avm (`~/projects/aice-avm`, localhost:8080)
- 稼働中。GitHub `yaskodama/aice-avm` main = **`f98d008`**(全push済):
  - `ed179b0`: 遠隔再起動 `/api/restart` + MCC自動操縦(常時監視/自動参加/性能表`__MESH_PERF`/sim監視センター可視化 + sim行↻Restart)
  - `f98d008`: Drone Swarm Supervisor(負荷対応ドローン配置 P1、🚁ボタン)
- 起動: `cd ~/projects/aice-avm && ./_build/default/server.exe 8080 --no-open &`(ビルド=`dune build`)

### Windows sim (192.168.3.32:8080)
- ★**要 start.bat 再実行**: 最後の確認で `/api/restart` が空応答=旧コードのまま。GitHub反映済なので start.bat 再実行→自動pull→`/api/restart`有効化。以降MCCの↻Restartで遠隔再起動可。AIPL計算=373ms実測済(Mac sim 183msの2倍)

## 2. ★最優先の未完: マルチコア描画相互作用クラッシュ (task#10)

**検証済**: par ON + wait()無しテスト(軽200/中5万/重50万反復)は全て **ボード生存・nbatch増加・並列発動正常**。リグレッション無し。→ **並列ロジックは正しい**。

**未解決**: 並列バッチ → **同一tick内で `avm_render`(これもsmp_parallel_sumでラスタバンド分散)** を呼ぶ wait()付きテストで **core0ハードクラッシュ(ping断)**。

**有力仮説**: `system/smp.c` の `smp_parallel_sum` の **SMP_WAIT_LIMIT(2e9≈1-2s)takeover** が、D-cache OFFで重い(~5M ops=数秒)アクターdispatchを実行中の「遅いだけで生きているワーカー」を奪い、メールボックス(smp_job_seq/done)をdesync→次のSMP使用(ラスタ)でcore0クラッシュ。

**修正案(次回これを試す)**:
- `system/smp.c` に **takeoverしない fork-join** `smp_parallel_dispatch(fn,n,ncores)` を追加(avm_dispatchはguard 5M opsで有限時間に必ず終わるので、takeoverせず完了待ちで安全)。`device/video/avm.c` の並列バッチで `smp_parallel_sum`→`smp_parallel_dispatch` に差し替え
- 代替: 並列dispatchのop予算を小さく(例 200K)してtakeover閾値時間を超えさせない
- ★**シリアルコンソールでフォルト箇所(ESR/FAR)を観測**できると確実。各テストがボードをクラッシュさせ電源再投入要なので、シリアル接続推奨

**テスト手順(再焼き不要・実行時)**:
1. `/avm-par?on=1` で並列ON
2. `bench/multicore_test/*.abcl` を Mac `~/projects/aice-avm/_build/default/compile_avm.exe IN.abcl OUT.avm` で .avm 化
3. `curl -X POST --data-binary @OUT.avm "http://192.168.3.101/actor/loadvm?off=0"` → `curl "http://192.168.3.101/actor/loadvm?go=1&len=<N>"`
4. `/avm-par` で nbatch/lastbn 確認、`/run?cmd=help` で生存確認
- テスト群: `ParallelTestLight`(各200反復,描画無=生存OK), `ParTestRender`(wait付=現状クラッシュ→修正対象)
- ★ボードのPRINT(0x42)は無視される→結果確認は描画(line+wait)経由のみ。class方式は Mac compile.ml、ボード/actor/loadsrcのcc JITはfunction方式・型注釈必須(別方言)

## 3. 次にやれる選択肢(中断前の到達点)

- **(a) マルチコア描画クラッシュ修正**(task#10、上記smp_parallel_dispatch案)← 推奨
- **(b) ドローンP2**: Droneアクターを割当先ボードへ実分散spawn(aice-avm host側)
- **(c) 推奨順#3**: ボード間 直接メッシュ アクターRPC(Mac星型除去、各ボードのwebactor/tcp_serverにアクターRPC受信追加)
- **(d) rpi4/rpi3マルチコア展開**(rpi5バグ解決後)
- **(e) #5 D-cache rpi5**(最大速度向上だがDMAリスク高)

## 4. ★未コミット警告 (xinu-rpi5, main branch)

今セッションのカーネル作業は**未コミット**(ディスク上には在る)。特に:
- `device/video/avm.c`(**未追跡** = マルチコアスケジューラ本体), `include/avm.h`(未追跡)
- `system/mmu.c`(I-cache), `device/wifi/wifi.c`(メッシュ高速化), `system/tcp_server.c`(/avm-par + loadvm + loadsrc)
- `device/video/{video,wm,basic,basicwin}.c` 等も変更あり(一部は前セッション分混在)
- `compile/kernel_2712.img.*`(ビルドバックアップ、無視可)

→ 再開時、まず `git add` でコミット推奨(main直push避けるなら feature branch)。稼働カーネル md5 = `7cbf0f3f`。

## 5. 性能比較表(実測値、参考)

| ノード | CPU | 1-core primes300k | N-core | 備考 |
|---|---|---|---|---|
| Mac sim host | M4 | 28ms(native) | — | AIPL interp 2Mloop=183ms / lat 0.3ms |
| Win sim host | x86_64 | — | — | AIPL interp=373ms / 要start.bat再実行 |
| rpi5 | A76×4 | 67ms | 22ms(3.0x) | I-cache後53倍改善・mesh 10.0.0.3 |
| rpi4 | A72×4 | 158ms | 53ms(3.0x) | blender表示 |
| rpi3 | A53 | (SMP無) | — | 単一スレッドwebactor |

実機JIT vs シミュレータ: 生CPUはMac/Win 2-6倍速だが、simはAIPLインタプリタ実行(~100倍overhead)ゆえ **実機Xinu(JIT)が実効アクター実行 約10-50倍速**。sim=監視/調整、実機=計算ノード が最適配置。
