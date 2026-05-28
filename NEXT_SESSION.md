# NEXT_SESSION — xinu-rpi4

## ✅ 2026-05-28 — `now` = プロセス間同期 RPC

メソッド内 `now obj.m(args)` を `cc_call(self,to,mid,args)`→`ap_call` に: 呼出元アクターを
`ap_select(AP_REPLY=-2)` でブロック→相手プロセスが処理し戻り値を AP_REPLY で返信→再開し値取得。
`ap_msg.reply_to`+`ap_post`、actor_proc_main が reply_to>=0 で dispatch 戻り値を返信。トップレベル
`now` はインライン dispatch のまま(main はアクタープロセスでない)。QEMU で Client.go の
`now srv.query(5)`→ブロック→`got 50`、Counter top-level now(5/42/42)回帰 OK。
commit xinu 1242109 / abclcp e289923。examples_xinujit/Rpc.abcl。**未 flash**(実機最新 50d6ad13)。

## ✅ 2026-05-28 — AIPL `select` 統合 (actors as Xinu processes)

--xinu-jit を actorproc 機構に配線完了: `g_spawn`→`cc_actor_new()`→`ap_spawn`(各 AIPL アクター=
Xinu プロセス+メールボックス)、`send`→`ap_send`、main()/各 /actor/send 後に `ap_run`(quiescent まで)、
one-shot /compile は `ap_killall`(proc_kill でスタック解放)・resident は保持。**`select{case m(v):}`→
`cc_select(self,n,m0..m3)`**(ブロッキング選択受信→matched method id、`cc_sel_arg` で引数、case 本体は
pattern var バインド)。翻訳器は send/now/select 参照の全メッセージ名にも id 割当。proc.c に proc_kill。
QEMU: Worker.run() の `select{add/stop}`→add 10/add 20/stopping、async(Ping/PingPong/MapReduce 4並行
→sum 14)回帰 OK。commit xinu 84fb673 / abclcp b36e351。examples_xinujit/Select.abcl。
★翻訳器変更後は **.c を再生成**(古い .c は g_spawn が cc_actor_new 未呼で無反応)。max 7 アクター(NPROC-1)。
`now` は同期 dispatch のまま。**未 flash**(実機最新 0d8f2140)。

## 🚧 2026-05-28 — actors-as-Xinu-processes 機構 (基盤)

`system/actorproc.c`: 各アクターを協調 Xinu プロセス(proc.c)化、アクター毎メールボックス +
ブロッキング受信。`ap_send`(enqueue+proc_ready)/`ap_recv`(空なら proc_block)/**`ap_select`(選択的
受信=指定メソッドを待ち他は残す→AIPL `select` の核)**/`actor_proc_main`(recv ループ→g_actor_dispatch)/
`ap_spawn`/`ap_run`。`proc.c` に **PR_WAIT + proc_block()(ready無しなら NULLPROC へ) + proc_create_arg()**。
シェル `actordemo`(2アクター ping-pong)/`selectdemo`(GO を DATA より先に select)で QEMU 検証。commit bee8b34。
★**機構のみ(native demo)。次段階=AIPL --xinu-jit 統合**: g_spawn でアクター毎 ap_spawn、`send`→ap_send、
翻訳器が `select{case m:}` を ap_select 呼出+分岐に出力。run-to-completion pump では不可能だった
「ハンドラ途中で特定メッセージを待つ」が可能に。未 flash(実機最新 0d8f2140)。

## ✅ 2026-05-28 — 非同期 mailbox (並行アクター)

AIPL `send obj.m(args)` を fire-and-forget 化。cc.c に FIFO キュー + 外部 `enqueue(to,mid,a0..a3)`
+ `cc_pump()`(g_active_dispatch を FIFO 呼出、runaway deadline で停止)。main()/各 /actor/send の後に
pump → ハンドラ内 send も連鎖処理。`now` は同期 dispatch のまま。翻訳器 Send→enqueue。QEMU で
自己送信カスケード(ping 3/2/1)・2アクター ping-pong(hit 4/3/2/1/0) 確認。examples_xinujit/Ping.abcl,
PingPong.abcl。★`now` 単独文はパースエラー→ドライバは send 推奨。commit xinu ce643f1 / abclcp bda137b。
**現 flash 1003e5bb には未搭載**(async は次回 flash)。

## ✅ 2026-05-28 — 常駐 AIPL アクター (load→メッセージ交換) + float 値

**常駐アクター**: `cc_actor_load(src)` が AIPL→C を常駐コンパイル(code+arena 非解放→g_obj/文字列
永続)、main() でアクター spawn。`cc_actor_send_msg(actor,method,arg)` が __method_id で名前→id 解決
→dispatch→結果 render。翻訳器が `__method_id`/`__nobj` を出力、codegen に `cc_func_offset`。
HTTP **POST /actor/load** / **GET /actor/send?to=&m=&arg=**、シェル `aload`/`amsg`、Mac 側
`examples_xinujit/actor_server.sh`。★メソッド名は 8整列バッファにコピーしてから v_str(奇数アドレスは
int タグと衝突)。vheap は load 時のみ reset(string/float フィールド永続)。QEMU(aload/amsg)で
Counter を load→bump 10=52→get=52→bump 100=152→get=152(状態永続) 確認。**HTTP 経路は実機要**
(現 flash 79b531b5 は未搭載)。commit xinu 3345537 / abclcp 9c30606 / 83d2905。

**float 値**: value_t に float variant(boxed double, bit60 タグ)。v_floatlit/promote/6桁表示。
cc.c のみ `-mgeneral-regs-only` 除外ビルド + CPACR FP 有効化。翻訳器 `Float→v_floatlit(IEEE bits)`。
QEMU で Rotor.spin(0.5*3=1.5)・"angle = "+2.0 OK。commit xinu 3336403 / abclcp dccc2f7。

## ✅ 2026-05-28 — AIPL --xinu-jit を value_t 化 (文字列対応, QEMU検証)

`cc/cc.c` に **タグ付き value_t ランタイム** (低位bit1=即値int / 0=文字列ポインタ)。
v_int/v_str/v_add(int加算|文字列連結)/v_sub/mul/div/lt/le/eq/ne/and/or/not/print/truthy/
int_of + 連結用8KBヒープ、cc_resolve_extern 公開。abclcp `c_translator.ml gen_program_xinujit`
を raw-int→value_t に retarget (リテラル→v_int/v_str、演算→v_*、print→v_print、if/while→
v_truthy、objイド→v_int_of)。**`print("count = " + n)` 等の文字列+連結が動作**、int は即値で
ループ安価。QEMU `cc` で String(count=5/42)・Counter(5/42/42)・Summer(5050)・Multi(123) 全 OK。
float は依然 int 切捨。commit: xinu-rpi4 `2cc615c` / abclcp `8197984`。残: float 値、async
mailbox、select/saga。

## 🚧 2026-05-28 — 仮想記憶 (MMU) Stage 1–3 完了 (QEMU検証・実機未flash)

MMU オフだった前提を反転。`system/mmu.c` + `include/mmu.h`、`kernel_main` の
`exception_init()` 直後に `mmu_init()`。3 ターゲット clean build (kernel8 111048 / 2712 98576 /
virt 98576)。**全段階 QEMU で検証済 (boot/shell/vfs/net/`cc` fib=55・actor=41、暴走 100ms 中断、vmtest)。**
- **Stage1 MMU有効化+identity+キャッシュ**: 39-bit VA / 4KiB granule。RAM=Normal WB inner-shareable、
  他=Device-nGnRnE+XN。MAIR/TCR(T0SZ=25,IPS=40bit,EPD1)/TTBR0 → SCTLR.M/C/I。identity で既存
  ポインタ不変・キャッシュ有効化(JIT 高速化)。
- **Stage2 W^X 保護**: 3 レベル(L1 1GiB / L2 2MiB / L3 4KiB)。カーネルの 2MiB をページ細分し
  `.text`=RO+X / `.rodata`=RO+NX / data・bss=RW+NX / heap=RW+X(JIT 用の例外)。link.ld/link_virt.ld に
  `_etext`+ページ整列追加。コード上書き不可・データ実行不可で起動&JIT 正常。
- **Stage3 VA→PA 変換実証**: `mmu_map_window()` が 32GiB の仮想窓 `VMAP_VA=0x800000000` を kmalloc
  したページにマップ。`vmtest` コマンド: VA に書いて PA から読む(逆も)→一致を確認 (translation works)。
- ★**DMA コヒーレンシ解決 = D-cache OFF / I-cache ON で運用** (`mmu.c`): D-cache を切ると
  データアクセスは RAM 直行 = 旧 MMU-off と同じく **GENET DMA / mailbox FB はコヒーレント**(壊さない)。
  I-cache は ON で命令フェッチ高速化(JIT の主因を改善)。TCR walk も Non-cacheable に。ページ表は
  RAM を Normal-cacheable のままにしてあるので、将来 DMA コヒーレント設計が出来たら C=1 に上げるだけ。
  → **実機フラッシュ可能な構成**(QEMU で D-cache off でも全機能 OK 確認済)。
- 残/次: (a) **実機フラッシュ + 検証**(MMU 下で GENET/`/compile` 生存=DMA コヒーレンシ確認)、
  (b) 将来 DMA コヒーレント設計で D-cache も有効化(非キャッシュ DMA セクション or cache maintenance)、
  (c) 保護違反を捕捉する recoverable 例外 (現ハンドラは halt 系で vmtest 以外の保護デモ不可)。
  ファイル: `system/mmu.c` `include/mmu.h` `loader/main.c` `shell/shell.c` `compile/link.ld`
  `compile/link_virt.ld`。**value_t/文字列対応 (task13/14) も保留中**。

## ✅ 2026-05-28 — JIT 暴走ループ保護 (★実時間ベース。反復予算版は実機ウェッジで失敗)

ネット公開 `/compile` は任意コードを **genet_rx_tick 内で同期 JIT 実行**するので、`while(1){}`
が Pi を固めるリスク。**ループ back-edge に打ち切りチェックを挿入**して防止:
- `cc/codegen.c`: while/for の back-edge (`b lbegin` 直前) に `emit_budget_check(lend)`
  = `bl __cc_tick; cbz x0, lend`。打ち切りで全ループが脱出→関数 return で unwind。
- `cc/cc.c`: `cc_tick()` は **実時間 (CNTPCT_EL0) で `CC_TIMEOUT_MS=100` を超えたら 0 を返す**
  (`cc_set_deadline()` を実行前に。`g_aborted` 時は出力に注記)。外部シンボル `__cc_tick`。
- ★★**反復回数ベース (2億) は失敗**: MMU/キャッシュ off で 1 反復が遅く、2億反復が実機で数十秒に。
  JIT は **genet_rx_tick 内で同期実行**するため、その間ディスパッチが止まり **GENET RX リングが溢れて
  永続ウェッジ** (実機 ping 100% loss → 要電源再投入)。QEMU はネット無しで露見せず。
  → **実時間打ち切り (~100ms)** に変更し、キャッシュ速度に依存せずディスパッチ停止を ~100ms に限定。
- **QEMU 検証 (時間版)**: `while(i>=0){i=i+1;}` → ~100ms で中断・注記・**シェル生存**、fib=55、
  Summer(while×100)=5050 回帰 OK。kernel8.img md5 `74956e27`。
- ⚠️ **要・実機 flash + 検証** (反復版 851d2baa が flash 済だが危険なので使わない)。flash 後は
  実機で while(1) を /compile に投げ、~100ms で aborted + 並行 ping 生存を確認すること。
- ★残: 深い無限再帰はスタックオーバーフローで fault しうる (loop 打ち切りでは捕捉外)。根本解決は
  S1 (preemptive timer) で JIT をディスパッチから切り離すこと。

## ✅ 2026-05-28 — AIPL→C 変換器を /compile 向けに retarget (実機検証済)

abclcp-project (`/Users/kodamay/ocaml-app/abclcp-project`) の AIPL→C 変換器に
**`--xinu-jit`** ターゲット (`src/c_translator.ml` の `gen_program_xinujit` + `src/aipl2c.ml`)
を追加。**整数アクター専用の自己完結サブセットC** (this kernel の `cc/` が受理する形:
`struct Obj{int cls;int f[N];} g_obj[64]` / フィールド=配列 / 送信=`dispatch(to,mid,a0..a3)` の
cls switch / メソッド=`int m_<C>_<m>(int self,a0..a3)` / globals→main、ランタイムは print/puts のみ)
を生成。**実機 Pi 4 の `/compile` に POST して検証済**: Counter→5/42/42、Summer(while)→5050、
Multi(2アクター・クロス now・new)→123、並行 ping 0% loss。
- サンプル+スクリプト: `abclcp-project/examples_xinujit/` (Counter/Summer/Multi.abcl + run.sh + README)。
  ワークフロー: `examples_xinujit/run.sh File.abcl [host]` = 変換→`/compile` POST。
- スコープ: int のみ (float→int 切捨、文字列は print リテラルのみ)。AIPL while は `while(c) do {…}`。
- 次: 文字列/float (value_t をカーネル側 externs 化)、async mailbox 化、select/saga、無限ループ保護。

## ✅ 2026-05-28 — 階層ファイルシステム + オンデバイス C コンパイラ (JIT)

AIPL→C→実行環境の土台として、Xinu 上に **(1) 使える階層 FS** と **(2) C を機械語に
コンパイルして即実行する JIT コンパイラ**を実装。**QEMU virt で全機能 PASS**、pi4/pi5/
qemu の 3 ターゲットともクリーンビルド (kernel8.img 96008 / 2712 85432 / virt 85176)。

### (1) 階層ファイルシステム
- `fs/vfs.c` + `include/vfs.h` に **cwd / 相対パス・`.`・`..` 解決 (`vfs_resolve`)、
  `vfs_resolve_parent` (parent+leaf 分離)、`vfs_path` (絶対パス生成)、`vfs_unlink`/
  `vfs_rmdir`/`vfs_child`** を追加 (既存の tmpfs ツリーの上に)。
- `shell/fscmd.c` (新規) に **pwd/ls/cd/mkdir/touch/cat/write/edit/rm/rmdir/tree/cp/mv**。
  `edit` は `uart_getline` ループで単独 `.` 終端の複数行入力 (piped stdin でも動く)。
  `shell/shell.c` の commandtab に登録。

### (2) オンデバイス C コンパイラ — 新コンポーネント `cc/` (Makefile COMPONENTS に追加)
- 対象サブセット: **int/char/ポインタ/配列/文字列リテラル**、`+ - * / %`・比較・`&& || !`・
  `& *`・代入、`if/else/while/for/return/{}`、**関数定義・引数・再帰**。
  ★`int` は **8 バイト** (AIPL value_t の 64bit タグ語と整合、レジスタ演算を X で統一)、
  `char`=1、ポインタ/配列=8。
- `cc/parse.c` = lexer + 再帰下降パーサ (型付き AST、ポインタ演算は要素サイズで scaling)。
- `cc/codegen.c` = **AArch64 機械語をバッファに直接生成するスタックマシン方式**。
  全エンコーディングは `aarch64-elf-as` で実機検証して手書き emit。式結果は x0、二項演算は
  rhs を SP に push して lhs 計算後に pop。ローカルは x29 からのフレームオフセット。
  ★JIT 前提で**絶対アドレスを emit 時に movz/movk で埋め込む** (文字列=arena アドレス、
  ユーザ関数=コードバッファ内オフセット (前方参照は fixup で後パッチ)、組み込み=カーネル関数)。
  分岐はラベル+fixup で相対 b/cbz/cbnz。
- `cc/cc.c` = ドライバ。arena(256KB) → `cc_lex`→`cc_parse`→`cc_codegen`(code 64KB) →
  **`dc cvau`/`ic ivau`/`dsb`/`isb` でキャッシュ整合** → `code+entry` を関数ポインタで**その場で実行**
  → 戻り値表示。組み込みは **外部シンボル表 `cc_resolve_extern`** で解決:
  `print(int) / putchar / puts(char*) / actor_send(id, "method", arg)`。
  ★`actor_send` が `system/actor.c` の `actor_message` を呼ぶ = **コンパイルした C から
  Xinu アクターランタイムを直接駆動** (AIPL バックエンドの seam)。
- シェル: `cc <file.c>`。`/examples/{fib,hello,actor}.c` を起動時に投入 (`loader/main.c`)。

### QEMU で確認した動作 (全 PASS)
- FS: pwd/ls/cd/相対パス/`..`/mkdir/write/cat/edit(複数行)/tree/cp/mv/rm/rmdir。
- `cc /examples/fib.c` → `0 1 1 2 3 5 8 13 21 34` + `=> 55` (再帰・呼び出し・制御フロー)。
- sum 1..100 → 5050、`int a[5]; a[i]=i*i; Σ` → 30, `a[4]`→16 (配列・添字・scaling)。
- `strlen(char*)` + 文字列リテラル + `puts` → `hello, xinu` / `=> 11`。
- `cc /examples/actor.c` → `actor_send` で counter を bump/add/get (1,41,=>41)。

### ⚠️ 実機での使い方の制約 (次の課題)
- 現状 Pi4 実機は **HDMI 接続時 `wm_run()` が main を占有**し UART REPL (`shell_main`) に
  到達しない + **USB キーボード未実装 (XHCI 未完)** なので、`cc`/FS を実機で対話駆動できない。
  → **(a) HDMI を外して起動すれば serial REPL で使える**、または
    **(b) HTTP に「C ソース受信→コンパイル→実行→結果返却」エンドポイントを足す**
    (= ユーザの言う **AIPL 動的コンパイル**そのもの。既存 `system/tcp_server.c` の HTTP
     gateway に `/compile` 等を追加すれば live 実機でネット越しに JIT 実行できる)。
- コンパイラ/FS は QEMU と Pi4 で**同一コードパス** (GENET/HDMI に非依存、`-mstrict-align`
  も共通) なので、実機でも同じく動くはず。要・上記いずれかの入力経路。
- 次の自然な発展: **HTTP `/compile` エンドポイント** → **`c_translator.ml --xinu` を本コンパイラの
  サブセット + `actor_send`/`print` ランタイムに retarget** → 本物の .abcl アクターを実機 JIT。

### コンパイラ言語拡張 (2026-05-28 同日、E1–E4 完了)
AIPL→C 出力 (value_t struct / `objects[].fields[]` / switch / typedef) に近づけるため拡張:
- **E1 グローバル変数** (scalar+array): トップレベル var 宣言を arena 内永続ストレージに置き
  絶対アドレスを movz/movk で埋込。ゼロ初期化 + 定数スカラ初期化。`find_var` は locals→globals。
- **E2 typedef + sizeof**: `typedef <type> <name>;` を型表登録、`declspec` が typedef 名を受理。
  `sizeof(type)` / `sizeof expr` を定数 int に (★int=8/char=1/ptr=8/struct=合計)。
- **E3 struct + メンバアクセス**: `struct Tag { ... };` 定義 (メンバ名/型/オフセット、★8byte
  メンバは 8 整列必須=MMU off の unaligned fault 回避)。`declspec` が `struct Tag` 受理。
  `a.b` / `a->b` (後者は `(*a).b` に正規化)。ネスト/配列メンバ/グローバル struct 配列 OK。
  ★値渡し/値返しは未対応 (ポインタ経由)。
- **E4 switch/case/default + break/continue**: switch は値比較連鎖 + `b.cond` ディスパッチ
  (b.cond エンコーディング追加)、case はラベル配置 (フォールスルーは C 準拠)。break/continue は
  ループ/switch のラベルスタックで解決 (for の continue は inc 点へ)。
- ★lexer に `struct/typedef/switch/case/default/break/continue` キーワード、`->`、`:` を追加。
- **QEMU 検証**: グローバル counter/配列/定数init (3/60/100→163)、struct (`objs[0].fields[1]` 等
  →25/7/300/109)、typedef+sizeof (8/1/16/42→30)、switch+break+continue (sum 23 / classify
  200/300/999)。既存 fib/strlen/actor も回帰 OK。3 ターゲット clean build
  (kernel8.img 98696 / 2712 90120 / virt 89864)。
- **コンパイラ対応 C サブセット (現状)**: int(8B)/char/ポインタ/配列/struct/typedef/文字列、
  四則+剰余・比較・`&&||!`・`&*`・`. ->`・添字・代入・sizeof、if/else/while/for/switch/
  return/break/continue/{}、関数・引数(≤8)・再帰、グローバル変数。組み込み
  print/putchar/puts/actor_send。**未対応**: struct 値渡し/値返し、複合リテラル、setjmp/longjmp
  (saga)、float、関数ポインタ、可変長引数、複数翻訳単位。

### HTTP /compile エンドポイント (動的コンパイル, 2026-05-28 同日)
**ネット越しに C ソースを送る→コンパイル→JIT 実行→結果(出力 + `=> 戻り値`)を返す**。
live 実機で AIPL/C を投げて実行できる = 動的コンパイルの seam。
- `include/cc.h` 新規 + `cc/cc.c`: **出力キャプチャ**追加 (`g_cap`/`emit_ch`)、コア
  `compile_run_core` を抽出、**`cc_run_source(src,len,out,outcap,*retval)`** 公開。
  `cmd_cc` もこの経路に統一 (→ キャプチャ経路を QEMU で検証可能、fib=55 確認済)。
  エラー時は out に `cc: <message>`。
- `system/tcp_server.c`: HTTP gateway に **`POST /compile`** (ボディ=C ソース) と
  **`GET /compile?src=<urlencoded>`** を追加。`cc_run_source` を呼び text/plain で結果返却。
  ★**リクエストのセグメント蓄積**を実装 (`g_httpreq[4096]`+`request_complete`: POST は
  Content-Length まで待つ、GET はヘッダ終端で完結)。これで C ソースが MSS を跨いでも受信可。
  SYN-ACK (finicky) は不変更。応答は 1 セグメント (≤~1400B、prog 出力は 1100B に cap)。
- ✅ **2026-05-28 実機 Pi 4 で /compile 完全検証** (kernel8.img de61f41f を flash・起動):
  `POST /compile` で inline(`=>7`)・fib(`=>55`)・strlen(`=>15`)・actor(`bump/add/get`=1/41/`=>41`)・
  struct+global+switch(25/119/`=>25`)、`GET ?src=`(urlencoded `42`) すべて成功。**HTTP でコンパイル
  した C が実機アクターを駆動** (`/api/actors` で counter value=41 msgs=3 永続)。**並行 ping 0% loss**
  (ICMP wedge 回帰なし)。886B ソース (複数セグメント) も `=>120` で**セグメント蓄積が機能**確認。
- ⚠️ 発見: **>4096B のソースは蓄積バッファ超過で応答せずタイムアウト** (ping/サーバは生存・後続接続は復帰)。
  → **修正済 (要・次回 flash)**: `g_httpreq` を **8192** に拡大 + バッファ満杯で未完なら **413 を返して close**
  (ハング回避)。現在 flash 済の de61f41f にはこの修正は未反映 (kernel8.img 再ビルド済 md5 1d9c27a5)。
- 実機検証手順 (Pi 4):
  ```sh
  cd compile && make pi4              # kernel8.img (100808 B)
  # SD を Mac に挿す → cp kernel8.img /Volumes/bootfs/ ; sync ; diskutil eject
  # Pi 起動後 (Mac は 192.168.3.202):
  sudo arp -s 192.168.3.100 d8:3a:dd:a7:fd:bf
  curl -s --data-binary 'int main(){ puts("hi over HTTP"); return 7; }' http://192.168.3.100/compile
  curl -s --data-binary @examples_fib.c http://192.168.3.100/compile
  curl -s --data-binary 'int main(){ return actor_send(0,"bump",0); }' http://192.168.3.100/compile
  ```
  期待: 出力 + `=> 7` / `=> 55` / `=> 1` 等。並行 ping 生存も確認。
- ★既知リスク: 投げた C が無限ループだと dispatch (genet_rx_tick) ごと固まる (将来は命令数上限を)。

### 主要ファイル
- `fs/vfs.c` `include/vfs.h` `shell/fscmd.c` `shell/shell.c`
- `cc/ccpriv.h` `cc/parse.c` `cc/codegen.c` `cc/cc.c` `include/cc.h` `compile/Makefile` (COMPONENTS に `cc`)
- `system/tcp_server.c` (HTTP `/compile`) `loader/main.c` (`/examples` 投入)

---

## ✅ 2026-05-27 (session 2) — HTTP actor gateway + Mac AIPL→Xinu actor 完動

**実機で「Mac の AIPL アクター → Xinu のアクターへ TCP/HTTP メッセージ」が完動。**
`python3 src/python-aipl/samples/xinu_http_actor_demo.py 192.168.3.100` →
`counter.bump→3 / add(40)→43 / get→43`、`store.set(7).add(35)→42`（値整合）。
シリアルに `SYN→SYN+ACK→ESTABLISHED (await HTTP request)→http: served request,
FIN sent`。**現在 SD カードに焼かれているカーネル: md5 `d466b1b`。**

### この session で land したもの（**未 commit**, 別途コミット予定）
1. **flicker 対策 = ダブルバッファ** (`device/video/video.c`, `wm.c`, `include/video.h`)
   - `fb_draw`(描画先)/`fb_back`(裏)/`video_present()`(裏→表一括コピー)、`wm_run` で
     `video_enable_backbuffer()`+毎フレーム `video_present()`。確保失敗時は直接描画。
   - 実機での flicker 解消は未確認（HTTP 機能を優先してテスト）。
2. **HTTP actor gateway** (`system/tcp_server.c`)
   - TCP listener を HTTP/1.0 化。`GET /` / `GET /api/actors` /
     `GET /send?to=N&m=<bump|add|set|get|reset>&arg=<n>`。ESTABLISHED の最初の
     data segment を request とみなし `http_build()` でルーティング、JSON 応答+FIN。
     port 80。libc 無いので s_put/s_putdec/q_param を内製。
3. **最小 actor 層** (`system/actor.c` + `include/actor.h`)
   - ACTOR_MAX=8、各 int field[4]+name。demo: id0=counter, id1=store。
     `actor_message(id,method,arg,*out)` 同期配信。**将来の AIPL codegen 受け皿**。
4. **Mac 側 AIPL `http://` scheme** (abclcp-project 側 `src/python-aipl/aipl_remote.py`)
   - `_is_xinu_http()`/`_xinu_http_send()`、remote_send/remote_call_sync を
     `GET /send?...` に翻訳。明示 `http://` prefix=Xinu gateway / bare host:port=
     AIPL JSON gateway。デモ: `src/python-aipl/samples/xinu_http_actor_demo.py`。
5. **★ `-mstrict-align` を COMMON CFLAGS に追加** (`compile/Makefile`) — **最重要**
   - actor_init() が `gratuitous ARP done` 直後でハングした真因 = GCC -O2 が
     `set_name`/`field[]`ゼロ化を不整列ワイドストアにマージ → **MMU-off
     Device-nGnRnE で data abort**（TCP seq/ack と同 class）。`-mstrict-align` で
     GCC が不整列アクセスを一切出さなくなり**この種の地雷を kernel 全体で根絶**。
     QEMU smoke PASS、退行なし。**A2 (AIPL 移植) の必須土台**。

### 再開時のテスト手順
```sh
# Mac
sudo arp -s 192.168.3.100 d8:3a:dd:a7:fd:bf
ping 192.168.3.100
curl http://192.168.3.100/api/actors
curl 'http://192.168.3.100/send?to=0&m=bump'
python3 src/python-aipl/samples/xinu_http_actor_demo.py 192.168.3.100
# シリアル: screen /dev/cu.usbserial-1120 115200   (抜ける: Ctrl-A K y)
# ビルド/焼き: cd compile && make pi4 ; cp kernel8.img /Volumes/bootfs/ ; sync
```

### 次の作業 = A2（本物の AIPL アクターを Xinu 上で）
`src/c_translator.ml --xinu`（abclcp-project、xinu-raz 向けの AIPL→C codegen）を
**xinu-rpi4 の actor/mailbox API 向けに retarget**し、.abcl のアクターを Xinu で
動かす。現状の Xinu actor は `system/actor.c` のネイティブ実装（object table 風で
受け皿になっている）。`-mstrict-align` 済なので不整列地雷は回避済み。

落とし穴メモ: ① 全 multi-byte パケット/構造体アクセスは `-mstrict-align` で安全化
済（個別 volatile はもう不要だが既存のは残置）② Pi 4 シリアルは uart.c の
GPIO14/15→ALT0 明示マックス必須 ③ Mac の static ARP は失効するので毎回 `arp -s`。

---

## ✅ 2026-05-27 — Pi 4 NET-G TCP 実機完走（解決）

`nc 192.168.3.100 8023` で **3-way handshake → ESTABLISHED → greeting
"Hello from xinu-rpi4 ..." 送信**を実機で確認（シリアルログに `tcp: SYN ...
-> SYN+ACK` / `tcp: ESTABLISHED`、以後も RX が流れ続け＝ハング解消）。

**長らくの「nc で全停止」の真因 = unaligned 32bit read の data abort:**
- `tcp_handle_packet` の `seq`/`ack` 読み `(tcp[4]<<24)|(tcp[5]<<16)|...` を
  GCC -O2 が big-endian 32bit load idiom と認識し単一 `ldr` にマージ。TCP seq は
  フレームオフセット **38（4 バイト境界外）**。**MMU off で DRAM が
  Device-nGnRnE** なので unaligned word load が **data abort → 無限ハング**
  （Frame/rx/ping 全停止）。ICMP には 32bit アライン外読みが無く ping は無事だった。
- **修正**: `system/tcp_server.c` の解析ポインタ `ip`/`tcp`/`data` を
  `const volatile unsigned char *` に（GCC のワイドロード・マージ禁止＝厳密バイト
  単位ロード）。net_responder の TX-frame stores と同じ Device-nGnRnE 対策の RX 版。
  同型の罠は `dhcp_client` の `got_xid` 読みにもある（DHCP 有効化時は要 volatile 化）。
- **シリアルが必須だった**: `uart.c` に GPIO14/15→ALT0 を明示ピンマックス
  （`-DGPIO_BASE=0xFE200000UL`）。Pi 4 は firmware が PL011 を BT に取り mini-UART を
  header に出すことがあり、これが無いとシリアルに何も出ない。配線: 黒=GND(pin6) /
  白=pin8(GPIO14 TXD) / 緑=pin10(GPIO15 RXD) / 赤(VCC)=未接続。115200 8N1。
- **デバッグ手法**: `genet_rx_tick` に RX マーカー D/R/r を仕込みシリアルで
  「ハング直前の最後の文字＝`D`（解析中の abort）」を特定 → 解決後マーカーは除去済み。

→ 以下は 2026-05-25 時点の記録（root cause 認識が「リング飽和」だったが、実際は
  上記 unaligned read だった。リング拡張/bounded loop 等は改善として残置）。

---

## NET-G TCP を dormant → **LIVE** 化（2026-05-25 記録）

前回 (`d9e9b81`) は「`tcp_handle_packet` を rx_tick から呼ぶだけで ICMP
responder が止まる」という未解決バグで TCP dispatch を OFF にしていた。
今回その**根本原因を特定して修正**し、TCP listener を常時稼働にした。

### 根本原因 — RX/TX リングの飽和 → 恒久 wedge

- dispatcher (`genet_rx_tick`) は **WM フレームごとに 1 回**しか走らない
  (`wm_run` が 6 ウィンドウを全描画する ~50ms/frame ≒ 20fps が律速。
  これが ping RTT 900ms の正体でもある)。
- 旧 RX リングは **16 descriptor (32KB) だけ**。`nc` 接続時の SYP burst +
  LAN broadcast + hot path 中の `uart_puts`(framebuffer console は激遅) が
  重なると、50ms の描画中にリングが埋まり、**on-chip RBUF FIFO が overflow
  → RX path が wedge**。ICMP echo request すら受信できなくなるので
  「responder が止まった」ように見えていた。
- TX 側も同様で、`genet_tx_frame` の timeout 時に ring を放置していたため、
  1 本詰まると以後の ICMP reply(TX) も全部詰まる ("TX ring degradation"、
  NET-F の DHCP で観測されていた症状と同根)。

### 修正内容 (commit 予定、未 commit)

| ファイル | 変更 |
|---|---|
| `device/genet/genet.c` | RX ring **16→256** (HW 全 descriptor)。`genet_rx_recover()` 追加 (RBUF flush + 全 desc 再 arm + CONS/SW index 再同期)。`genet_rx_poll` に overrun guard (`prod-cons > ring` で recover)。hot path の per-packet `uart_puts` 全撤去。`genet_tx_frame` timeout 時に ring resync + DMA 再 enable。overrun/recovery/tx_timeout カウンタ + accessor。 |
| `loader/main.c` | `genet_rx_tick` dispatch を `DHCP → TCP → ARP/ICMP` の clean な連鎖に。**`tcp_handle_packet` を LIVE 化**。boot で `tcp_set_mac()` + `tcp_listen(23)`。hot path のデバッグダンプ撤去。 |
| `system/tcp_server.c` | LISTEN で **SYN+ACK を実送出** + `TCP_SYN_RCVD` 遷移 (前回はコメントアウトで handshake 不成立だった)。 |
| `system/net_responder.c` | ICMP/ARP の per-packet ログを先頭 8 件で throttle (steady-state の hot path を高速化)。 |
| `shell/shell.c` | `rxstat` に `overruns= recoveries= tx_timeouts=` を追加。 |

### 副次的に直したビルド破損 (TCP とは無関係、要 commit)

`make clean` したら **コミット済みツリーが元々クリーンビルドできない**ことが
発覚 (Makefile にヘッダ依存追跡が無く、ヘッダ drift しても .c が再コンパイル
されず stale .o で動いていた)。以下を修正し、**pi4 / pi5 / qemu 全 3 変種が
0 error でクリーンビルド**するようにした:

- `include/memory.h`: `ROUNDMB`/`TRUNCMB` マクロを追加 (memory.c が使うが未定義
  だった)。`freemem` 宣言を `void`→`int` (定義と一致)。xinu-raz network 移植が
  `<memory.h>` 経由で要求する `struct memblock` を定義 (include 順で native
  memory.h が優先され incomplete type になっていた、13 ファイルが失敗)。
- `include/genet.h`: 非 pi4 (`#else`) 分岐に全 genet 関数の inert stub を追加
  (pi5/qemu の main.c/shell.c が無条件参照していた)。
- `shell/shell.c`: `_end` のローカル再宣言を削除 (memory.h と型衝突)。

## 検証状況

- ✅ **pi4 / pi5 / qemu フルクリーンビルド 0 error** (`make clean && make all`)。
  kernel8.img 67088 / kernel_2712.img 56400 / kernel_virt.img 56144 bytes。
- ✅ **QEMU virt smoke**: boot / shell / pingpong / procdemo / ps / halt 全 OK。
  `mem` で heap allocator (ROUNDMB/freemem 修正) が正常動作 (`_end` も正値)。
- ⏳ **GENET ネットワークは QEMU では検証不可** (virt に GENET 無し)。**実機
  Pi 4 で flash + ping/nc 検証が必要** ← 次のアクション。

## 実機テスト手順 (Pi 4)

```sh
# 1. ビルド (済) — 念のため
cd /Users/kodamay/projects/xinu-rpi4/compile && make pi4

# 2. SD に焼く
diskutil mount /dev/disk4s1            # bootfs
cp kernel8.img /Volumes/bootfs/
sync && diskutil eject /Volumes/bootfs
#   (または: make install_pi4 SDCARD=/Volumes)

# 3. Pi 4 を起動、Mac 側 ARP 固定 + ping (= 退行していないことを先に確認)
sudo arp -s 192.168.3.100 d8:3a:dd:a7:fd:bf
ping 192.168.3.100                     # ← まず ping が今まで通り通ること

# 4. TCP を試す (今回の本命)
nc 192.168.3.100 23
#   期待: 3-way handshake 成立 → "Hello from xinu-rpi4 (Pi 4, BCM2711)!"
#         → ENTER で echo + 接続 close
#   nc 実行中も別ターミナルで ping が生き続けること (= ICMP 干渉が消えたこと)
```

### 見るべきポイント

- UART0 (シリアルコンソール) に `tcp: SYN from ... -> SYN+ACK` →
  `tcp: ESTABLISHED` → (ENTER 後) `tcp: FIN sent (got newline)` が出る。
- shell で `rxstat` を叩き、`overruns= / recoveries= / tx_timeouts=` を確認。
  - **理想は全部 0**。`recoveries` が少し増える程度なら自己回復が効いている証拠。
  - `tx_timeouts` が増え続けるなら TX ring がまだ不安定 (下記フォローアップ)。

## まだ残っている可能性 / フォローアップ

1. **実機で本当に直ったかは未確認** (QEMU で GENET を出せないため)。もし実機で
   まだ ping が止まるなら、`rxstat` の overruns/recoveries/tx_timeouts の伸び方で
   RX 飽和か TX wedge かを切り分けられる (今回そのための計器を仕込んだ)。
2. **RX を WM フレームより高頻度で drain したい**: 本質的には dispatch が
   20fps に縛られているのが弱点。S1 (GIC + Generic Timer の preemptive) が入れば
   タイマ割り込みから RX を回せて飽和耐性が一段上がる。
3. **DHCP (NET-F)** は依然 dispatch OFF (`dhcp_send_discover` は boot から外して
   ある)。TX ring recovery が入ったので連続 DISCOVER の劣化は緩和されているはず
   — 再挑戦の余地あり。`sudo tcpdump -i en0 -nn 'port 67 or 68'` で OFFER の
   戻り経路を切り分けるのが先。
4. **Makefile にヘッダ依存追跡が無い**問題は未対処。`-MMD -MP` + `-include
   $(DEPS)` を足すと、今回のような「ヘッダ直しても再コンパイルされず stale .o」
   の地雷が消える。次の小タスク候補。

## ファイル位置

```
リポ: /Users/kodamay/projects/xinu-rpi4/   branch: main
変更: device/genet/genet.c, include/genet.h, include/memory.h,
      loader/main.c, shell/shell.c, system/net_responder.c,
      system/tcp_server.c   (全て未 commit)
```

## Round 1 の他の残作業 (参考、優先順は実機 TCP 検証の後)

- **S1 preemptive timer** (GIC + Generic Timer IRQ → preemptive scheduler)
- **X1 AIPL hello** (Py-I or JS-O runtime を xinu-rpi4 上で hello-world)
- **XHCI-B〜H** (Pi 4 USB-A キーボード/マウス: VL805 enumeration)
- **N0 RP1 discover** (Pi 5 に切り替えるなら)
