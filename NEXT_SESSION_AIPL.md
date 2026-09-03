# 次回の再開メモ — Xinu Pi3 / Pi5 の AIPL 実行系

最終更新: 2026-09-04

## 0. いまの状態（両機とも生きている）

```
curl -s http://192.168.3.101/            # Pi5（80番）
curl -s http://192.168.3.50:8080/api/console   # Pi3（8080番）→ JSON が返れば新カーネル
```

| | Pi 3 | Pi 5 |
|---|---|---|
| IP / ポート | `192.168.3.50` : **8080** | `192.168.3.101` : **80** |
| リポジトリ | `~/projects/xinu-rpi3` (`arm-rpi3-port`) | `~/projects/xinu-rpi5` (`manet-m14-udp`) |
| 焼いてある | **`3df7543f…`**（2,163,096 B） | **`5aaf848f…`**（2,040,984 B） |
| 実行方式 | `.avm` をカーネル内 VM が解釈 | AIPL→C→機内 `cc` が AArch64 に JIT |
| アクタの器 | **Xinu の実プロセス**（本物の並行） | 協調ポンプ（単一スレッド） |
| 前段 | Mac の `compile_avm`（`~/projects/aice-avm`） | **機内**の `cc/abcl2c.c` |
| 投入 | `POST /actor/loadvm?ask=0` | `POST /cc`（AIPL をそのまま） |
| **出力を読む** | **`GET /api/console`**（今回追加） | `/cc` の応答にそのまま出る |
| 戻り値を読む | `web_expose` + `/api/x/<path>?method=&args=` | 同じ／応答にも出る |
| 正典ガイド 10 本 | **8 本が完全一致**（実機・出力照合） | **8 本が完全一致**（シェル経路なら 9） |

**2026-09-04 に両機の機能を揃えた。** 残る差は 3 つだけで、いずれも実行の器の性質:

| | 中身 |
|---|---|
| g7 の `slow(timed out)` | Pi 3 = 0（正典）。Pi 5 は **`/cc` では `wait()` が使えない**ので 42。**シェル経路 `/run?cmd=cc%20/home/g7.abcl` なら 0 で一致**。`/cc` は割り込みを塞いだ RX 処理の中で走るため（JIT を ISR の外へ出すのが前提） |
| g8 の `b = 1` | **両機とも同じ**（正典は `b = true`）。真偽値を int で持つため。Pi3/Pi5 の差ではない |
| g9 の順序 | Pi 3 は `got actor / pong / after send`（実プロセスなので send の相手が先に走る）。Pi 5 は正典どおり。**どちらも AIPL としては正しい**（send は順序を約束しない） |

## 1. 2026-09-04 にやったこと

**ガイド第39章（`~/aios/abclcp/docs/aipl_guide_{ja,en}.tex`）が差を全部書いていた。
それを潰した。** 日46頁／英41頁に改訂済み・コミット済み（`9da9a58`）。

| 直したもの | どこ | 効果 |
|---|---|---|
| `select` を Pi 3 と同じ継続分割＋横取りに | `xinu-rpi5 cc/abcl2c.c`（`8f70530`） | g4 が正典どおり。**ガイドが「唯一の残差」と書いていた項目** |
| `select` の `timeout` 節（`__sel_sweep`） | 同上 | 誰も来なければ `timed out` |
| トップレベル `send` の直後に FIFO を配る | 同上＋`cc/cc.c` の `cc_pump_now` | 配送順を Pi 3 に揃える（これが無いと横取りが効かない） |
| シェルの cc ランナー完了待ち | `shell/shell.c`, `system/proc.c` | `wait()` でランナーが眠っても取りこぼさない |
| `new C()` で `init()` が黙って走らない | `cc/abcl2c.c`（`3cf026e`）＋ `aice-avm compile.ml` | **両機とも同じ穴だった** |
| 期限切れで見捨てた返信を次の継続が受け取る | `aice-avm compile.ml`（`b0c28d3`） | g7 の `await` が 42 → **10**（正典） |
| `GET /api/console` | `xinu-rpi3`（`1d72b30`） | Pi 3 の出力が HTTP で読める |
| `init` の二重呼び（`create_obj_noinit`） | 同上 | g1 の余分な `hello, 0` が消えた |
| 新しいモジュールで前のアクタを畳む | 同上 | プロセス表の食いつぶしと HTTP 詰まりが消えた |

### 踏んだ穴（重要）

- **アクタ・スレッドを `kill()` してはいけない。** spawn 命令の中で `alloc_obj` が
  `objects_mu` を握るので、その最中に殺すと以後の生成が全部止まり、webactor まで
  巻き込んで **HTTP が詰まる**（ping は通るので故障に見える）。印をつけて起こすだけにする。
- **Pi 3 は立て続けの HTTP に弱い。** 1 プログラムあたり 2 リクエスト・10 秒間隔で安定した。
  3 リクエスト・8 秒間隔だと 6 本目で詰まった。
- **観測できないものは正しいと言えない。** `/api/console` を入れた初日に、
  それまで 1 か月見えていなかった欠陥が 2 件出た。

### 検証の道具

```
# Pi5: 生の AIPL をそのまま投げる（3 秒間隔）
curl --data-binary @g4_select.aipl "http://192.168.3.101/cc"
# Pi5: wait が要るものはシェル経路
curl --data-binary @g7.aipl "http://192.168.3.101/fs/write/home/g7.abcl"
curl "http://192.168.3.101/run?cmd=cc%20/home/g7.abcl"
# Pi3: .avm に落としてから（10 秒間隔）
~/projects/aice-avm/_build/default/compile_avm.exe X.aipl X.avm
curl --data-binary @X.avm "http://192.168.3.50:8080/actor/loadvm?ask=0"
curl "http://192.168.3.50:8080/api/console?clear=1"
# 正典の答え合わせ
cd ~/aios/abclcp && printf 'load docs/samples/guide/g4_select.aipl\ncompile\n' > /tmp/r.repl \
  && ./src/abclrepl_thread -q -f /tmp/r.repl < /dev/null
```

台本: `/tmp/g5x/run_raw.sh`（Pi5 生 AIPL）、`/tmp/g5x/run_pi3b.sh`（Pi3）。

## 2. 次の一手

1. **`/cc` でも `wait()` を使えるようにする**（g7 が全経路で一致する）。
   JIT を ISR の外へ出す＝HTTP ハンドラが専用プロセスを立てて完了を待つ形。
   シェル経路（`shell/shell.c`）に動く実装があるので、そこを HTTP へ持ち込む。
   **ただし `/cc` は割り込みを塞いだ RX 処理の中で走るので、応答の返し方から
   考え直す必要がある。** 過去 2 回、実行モデルを触って板を固めている。
2. 真偽値を `true` / `false` で印字する（両機とも `1`）。値にタグが要るので
   Pi 3 は VM 変更＝焼き直し、Pi 5 はランタイム変更。
3. Pi 3 の `ai_call` が遅くなる件（未解明。起動直後 2--6 秒 → 4.4 秒を実測。
   常駐アクタが増えるほど遅い、が有力だが切り分けていない）

## 2. 未完了

### A. Pi5 に `870ea069…` を焼く（最優先）

いま焼いてある `dc700cf9…` は、**間違った入力を投げると板が止まる**。
（正しい入力なら `ai_call` も引数つき `new` も動く。）

**焼く前に必ず**: `cc.c` を変えていなければ、生成 C を**現行カーネルの `cc`**
に直接通して検査できる。前回これを飛ばして回帰させた。

```
cc -DABCL2C_HOST_TEST -w -o /tmp/a2c ~/projects/xinu-rpi5/cc/abcl2c.c
/tmp/a2c prog.abcl > prog.c
curl --data-binary @prog.c "http://192.168.3.101/cc"     # ← 実機の cc で検査
```

焼き方（**`make install_pi5` は禁止**＝`config.txt` を上書きする）:

```
cd ~/projects/xinu-rpi5/compile && make pi5     # ★`make` 単体は何もしない
cp compile/kernel_2712.img /Volumes/bootfs/kernel_2712.img   # これだけ
# 退避を取り、md5 を照合し、config.txt が無傷か確かめてから eject
```

焼いたあとに確認すること:
1. `ai_call`（`/tmp/sage5.abcl` 相当）— 前回ここで回帰した
2. 引数つき `new` が `init` を呼ぶ（`init got x` が出る）
3. **板を止めていた入力をわざと投げる** — `future` / `timeout` / `select` /
   `reply` / `replyto` / `ai_cal`（綴り誤り）/ `init` 無しで引数。
   すべて名前つきエラーで即座に返り、**板が止まらない**こと
4. 回帰 — `/api/actors`、`/cc` に C、`/smp-bench`

戻すなら `/Volumes/bootfs/kernel_2712.img.bak-pre-noproto-42b53ee5…`（ひとつ前）か
`…bak-pre-a2cfix-7acc8185…`（`ai_call` が動いていた版）。

### B. Pi3 の `ai_call` が遅くなる件（未解明）

起動直後 2〜6 秒 → モジュールを 13 本投入したあと **16.6 秒**。
生成はトークンごとにスケジューラへ譲るので常駐アクターが増えるほど遅い、
というのが有力だが **切り分けていない**。測るなら状態を揃えること。

### C. Pi5 に文字列まわりの VM 変更は不要（確認済み）

以前「Pi5 に2つの VM 変更が未反映」と書いたが、Pi5 は `.avm` VM を使わないので
そもそも関係ない。`v_str`／`v_add`（連結）は元からある。

## 3. 落とし穴（踏んだもの）

- **Pi3 はモジュールを1本しか持てない**。`static unsigned char vm_mod[]` を
  `loadvm` が上書きする。古い公開ルートは `/api/routes` に残るが**指し先が死ぬ**。
  無応答を見たら、まず新しいモジュールを入れて呼ぶ（応えれば板は正常）。
- **`?ask=0` を落とすと Pi3 の `loadvm` は無応答**。板の故障に見える。
- **Pi5 の機内 `cc` は `int main(void)` を受け付けない**（`int main()` なら通る）。
  回帰と間違えやすい。**プロトタイプ宣言も持たない**。
- **`.gitignore` の `*.bin` がモデルを落とす**。`.incbin` の対象なので `git add -f`。
- **ARM32 では `unsigned long` が 32 ビット**。AArch64 から移すとき共用体で
  `double` を作ると黙って壊れる（Pi3 で `<unk>` だけを出し続けた）。
- **解析の失敗が「ノード番号 0」を返す設計は危ない**。0 が実在のノードだと
  無限再帰する。ベアメタルではそれがカーネル停止になる。

## 4. 検証の道具

- **Mac で `llm.c` を動かす台**（実機の答え合わせ用）:
  `/private/tmp/claude-501/.../scratchpad/llmtest/`（`harness.c`＋`shims.c`）。
  セッションの一時領域なので**消えている可能性がある**。作り直しは
  `llm.c` から `#include` を落として `kmalloc`/`kprintf`/`uart_*` を差し替えるだけ。
  参照出力: `ai_call("Once upon a time")` →
  `, there was a little girl named Lily. She loved to play outside in the p`（72 B）
- **`abcl2c` の掃き出し**: `-DABCL2C_HOST_TEST` でホスト単体翻訳器になる。
  `-fsanitize=address` を付けて、元ソースをランダムに切る/消す/重ねる/入れ替える
  変異を数百件流す。**1件の crash が板の停止**なので、代表例だけでは足りない。

## 5. 文書

- **AIPL ユーザーズガイド**: `~/aios/abclcp/docs/aipl_guide_{ja,en}.tex`
  （日本語 41 ページ／英語 36 ページ）。第39章が実機の節で、Pi3 と Pi5 の
  両方＋対照表＋実測した対応範囲の表が入っている。`lualatex` を 2 回。
- Pi5 のシステム取扱説明書は別物: `~/projects/xinu-rpi5/USERS_MANUAL_{JA,EN}.md`。
  **`ai_call` / `llm` の記述はまだ無い**（足すなら第7章シェルコマンドと
  第8章リモート操作のあたり）。

## 6. このセッションのコミット

```
xinu-rpi3   ec30a1b  Pi3 に小型LLM、ai_call を実機で通す（l_exp の 32/64 ビット）
aice-avm    ae97a3c  ai_call 実装、正典ガイド 10/10
xinu-rpi5   9d1420b  Pi5 にも機内モデル、ai_call
            da8fee8  abcl2c: ループを潰し未対応構文を名前つきで断る
            2fda833  abcl2c: 真因はノード番号0の無限再帰。プロトタイプ宣言も外す
abclcp      e8c75b4  ガイドに実機の節を新設
            77c332e  Pi3+Pi5 両対応、Pi3 の単一モジュール制約
            f33c3fe  Pi5 の実装を本格的に追記
```

---

## 2026-09-02 追記 — 「最新 AIPL への追従」の実装を確認した（コードは変えていない）

**前段は2つあり、追従しているのは Mac 側だけ。**

| 経路 | 前段 | 投入 | 正典ガイド10本 |
|---|---|---|---|
| Mac 側 | `~/ocaml-app/abclcp-project` の `aipl2c --xinu-jit` | `POST /actor/load` | `--check` **7/10**、C 生成 **8/10** |
| 機内 | `~/projects/xinu-rpi5/cc/abcl2c.c` | `POST /cc`（本文が `class`） | **0/10** |

### 1. Mac 側前段：記録どおり。回帰なし。**もうコミットされている**
`7a09d3e aipl2c の前段を現行 AIPL の表層構文へ追従させる`。
（このメモには「未コミット」と書いてあったが、コミット済み。）
`--check` は 7/10、落ちるのも同じ3本（g7・g10 = 後置 `timeout`、g3 = 型検査）。
`--no-typecheck` を付けると g3 も C まで出るので 8/10。

### 2. 機内 abcl2c.c：未着手。10本とも**同じ1箇所**で落ちる
`cc/abcl2c.c:124-126` — 引数リストの `)` の直後にいきなり `{` を要求している。
そのため `: T`（戻り値注釈）も `!{...}`（効果注釈）も受け付けず、全部
`method: expected '{'`。ほかに:

- **`++` を字句に持たない**（2文字演算子は `== != <= >= && ||` だけ。`abcl2c.c:55-56`）→ `expected expression`
- **`true` / `false` がキーワードでない** → 識別子として素通しし、生成Cが未定義の
  `v_true` を参照する。**ここは黙って通り抜ける**（実機の `cc` まで行って初めて落ちる）

### 3. 実機で裏を取った（192.168.3.101・板は生存）
| 投げたもの | 応答 |
|---|---|
| 旧方言 AIPL（`var`/`new`/`send`/算術） | `42` `=> 0` |
| `true` を含む生成C | `cc: undefined variable` |
| `method m() : unit` の AIPL | `abcl2c: method: expected '{'` |
| g1_hello の `--xinu-jit` C を `/actor/load` | `tick 1` `tick 2` |
| g8_bool_equality の同上 | `literal true ok` `literal false ok` `b = 1` `not-eq: 1` |

→ 焼いてあるカーネルの abcl2c は、手元の `cc/abcl2c.c` と同じ振る舞いをする。

### 4. ★ Mac 側経路に、記録に無かった**黙った欠落が2つ**ある
`--check` は通り、C も出て、実機も受け取る。それでも意味が消えている。

1. **`reply(...)` が `/* unsupported call */ return 0;` に潰れる。** 6本で計15箇所。
   g8 の `get()` は値を返さない。
2. **`new C(引数)` が `g_spawn(cls)` になり、引数も `init` 呼び出しも消える。**
   マーカーも警告も出ない。`m_Greeter_init` は生成されているのに誰も呼ばない。
   実機で g1 を載せると **`hello, AIPL` が出ず `tick 1` `tick 2` だけ**になる（実測）。

→ [[feedback-aipl2c-silent-drops]] と同じ形。**`--check` が通ることは動作の証明ではない。**

### 5. 忠実度の差（欠落ではないが記録しておく）
g8 は実機で `b = 1` を出す。正典なら `b = true`。
Xinu バックエンドは bool を int で持つため（`Int 1`/`Int 0` に写している）。

### 次の一手（未着手）
- 機内 `abcl2c.c`: メソッド署名で `: T` と `!{...}` を読み飛ばす（構造パスなので数行）。
  `++` を2文字演算子表へ。`true`/`false` を 1/0 のリテラルに。
  ただし `reply` は元から非対応なので、これだけではガイドは通らない。
- Mac 側: `new` の引数と `init` を落とさない。落とせないなら**名前つきで断る**。

---

## 2026-09-02（続き）— 機内 abcl2c.c を正典の表層構文へ追従させた

**`~/projects/xinu-rpi5/cc/abcl2c.c` を修正（+143/-7 行）。未コミット。**
正典ガイド標本で **0/10 → 5/10**（g1・g3・g6・g8・g9）。
残る5本は `future` / `select` / `timeout` / `web_listen` / `wait` で、
どれも**この装置に実際に無い機能**。名前つきで断る。

### 入れたもの

| | 実装 |
|---|---|
| `: T` 戻り値注釈、旧 `-> T` | `skip_method_annots()`。型は使わないので読み飛ばす |
| `!{log, mut}` 効果注釈、`@ N` 義務レベル | 同上。受理して捨てる（意味検査は正典の担当） |
| `x: D` 仮引数の型注釈 | 以前は `D` を2つめの仮引数として数えていた |
| `var n: int = 0;` | フィールドの型注釈 |
| `++` 文字列連結 | 2文字演算子に追加し、`parse_concat` を 比較 と 加減 の間に。`v_add` が連結する（`cc.c:365`） |
| `true` / `false` | `1` / `0` に。以前は識別子として素通しし未定義の `v_true` を生んでいた |
| `reply(e)` | **`return e;` と同じ**。`now o.m()` は `dispatch()` の返り値を受けるため |

### 直したバグ（どちらも実機を落とす／黙る類）

1. **`new <知らないクラス>` で `CLS[-1]` を読んでいた。** `class_idx` の -1 を
   そのまま構文木に入れていた。ホストでは ASan が捕まえるが、**実機では領域外参照**。
   ファズが見つけた。最小再現は `var x = new Zzz("q");`。名前つきで断るようにした。
2. **トップレベルの `var` が `main` のローカルだった。** メソッドから参照すると
   宣言の無い名前になり、生成 C が機内 cc で `undefined variable`。
   `scan_topvars()` で大域として先に宣言するようにした（g3 の `now stock.take(k)`）。

### 意図的に厳しくしたところ
- **フィールド初期値は int / bool リテラルのみ**。以前は「数値でなければ黙って 0」で、
  文字列や式の初期値が何も言わずに 0 になっていた。今は名前つきで断る。
- `reply` の後ろに文が続く場合は断る（`return` なので意味がずれるため）。

### 実機で確認（192.168.3.101・生成Cを `POST /cc` へ。板は生存）
| 標本 | 出力 |
|---|---|
| p5_old（旧方言・回帰） | `42` |
| g1_hello | `hello, AIPL` `tick 1` `tick 2` |
| g9_actor_arg | `got actor` `after send` `pong` |
| g8_bool_equality | `literal true ok` `literal false ok` `b = 1` `not-eq: 1` |
| g3_actors | `ok, left=7` `sold out` |
| g6_effects | `1` `1` `n = 1` |
| `-> T` と `@ N` の試験 | `42` |

いずれも正典の期待どおり。`b = 1` だけは正典なら `b = true`（bool を int で持つ差）。

### 回帰確認
リポジトリ内の `.abcl` 7 本は**修正前に通っていたものが 1 本も落ちていない**
（元から `unknown function` などで落ちる。Mesh 2本はエラーの出る場所が早くなっただけ）。

### まだやっていないこと
- **焼いていない。** `compile/kernel_2712.img` = `edb1a254…`（2,024,584 B）をビルド済み。
  ひとつ前の `870ea069…` は `compile/kernel_2712.img.pre-a2c-syntax-870ea069…` に退避。
  焼くと `POST /cc` に **AIPL のまま**投げられるようになる（今は生成 C を投げて確認した）。
- **コミットしていない。**

### 測り方（次回もこれで）
```
cc -DABCL2C_HOST_TEST -w -o /tmp/a2c ~/projects/xinu-rpi5/cc/abcl2c.c
for f in ~/aios/abclcp/docs/samples/guide/g*.aipl; do /tmp/a2c $f >/dev/null || echo "NG $f"; done
```

### ファズ（実機を止めないことの確認）
`-DABCL2C_HOST_TEST -fsanitize=address,undefined` で作り、ガイド標本と試験片を
切る／消す／重ねる／記号を差し込む／文字を入れ替える変異にかけた。
**6,000 件で crash=0 / hang=0。** 修正前のビルドではこの掃き出しが
`new: unknown class` の領域外参照を1件見つけている（上記）。

---

## 2026-09-02（続き2）— 焼いて、実機で確認した

**焼いた: `edb1a254…`（2,024,584 B）。** カード上の退避は
`kernel_2712.img.bak-pre-a2c-syntax-dc700cf9…`（＝それまで動いていた版）。
`config.txt` は `388a3a1a…` のまま無傷。
※ カードに載っていたのは `dc700cf9…` で、**`870ea069…` は結局焼かれていなかった**。

### 通ったこと

| 確認 | 結果 |
|---|---|
| 正典 AIPL を `POST /cc` に直接（g1・g3・g8・g9・g6） | すべて期待どおりの出力 |
| `ai_call`（機内LLM） | 参照出力と一致（72 B）、**0.42 秒** |
| 以前 板を止めていた入力 8 種 | **全部が名前つきエラーで即返り、毎回 板は生存** |
| 旧方言（回帰） | `42` |
| C を `/cc`（回帰） | `0 1 2` |
| `/smp-bench?n=200000`（回帰） | 4 コア、38ms → 12ms、3.16 倍 |

投げた 8 種は `future` / `timeout` / `select` / `replyto` / `ai_cal`（綴り誤り）/
`init` の無いクラスに引数つき `new` / `new <知らないクラス>` / `reply` の後ろに文。

### ★ まだ残っている段差 — `/cc` の振り分け（`system/tcp_server.c:1294`）

```c
if (bodyp[0]=='c'&&bodyp[1]=='l'&&bodyp[2]=='a'&&bodyp[3]=='s'&&
    bodyp[4]=='s'&&(bodyp[5]==' '||bodyp[5]=='\t')) {   /* → abcl2c */
```

**本文が literal に `class ` で始まるときしか AIPL と見なさない。**
正典のガイド標本は全部 `// g1: …` というコメントで始まるので、**C として扱われ**
`cc: expected type` になる。翻訳器の問題ではない（先頭コメントを外せば全部通った）。

直すなら、判定の前に空白と `//` `/* */` を読み飛ばしてから `class` を見る。
**abcl2c.c ではなく `system/tcp_server.c` の変更なので、焼き直しが要る。**

### 状態
- `cc/abcl2c.c` は**未コミット**。カーネルは焼き済み。

---

## 2026-09-02（続き3）— `/cc` の振り分けも直して焼いた。**ここが到達点**

**焼いた: `8773feb9…`（2,024,584 B）。** カード上の退避:
- `kernel_2712.img.bak-pre-ccdetect-edb1a254…`（abcl2c 修正のみの版）
- `kernel_2712.img.bak-pre-a2c-syntax-dc700cf9…`（それ以前）

`config.txt` は `388a3a1a…` のまま無傷。

### 直したもの（`system/tcp_server.c`、+22/-12 行。未コミット）
判定の前に空白と `//` `/* */` を読み飛ばしてから `class` を見るようにした。
abcl2c には本文をそのまま渡す（abcl2c の字句解析器がコメントを扱う）。
判定ロジックは単体で 15 ケース検証（未終端コメントでも読み過ぎない、ASan clean）。

### 実機で確認したこと（すべて 192.168.3.101、板は最後まで生存）

| 確認 | 結果 |
|---|---|
| **正典 AIPL を無加工で `POST /cc`** g1・g3・g8・g9・g6 | すべて期待どおり |
| 通らない 5 本（g2・g4・g5・g7・g10） | `future` / `select` / `web_listen` / `wait` / `timeout` と**名前つきで断る** |
| 先頭がコメントの **C** | これまでどおり C として通る（`7`） |
| 以前 板を止めていた入力 8 種 | 全部が名前つきエラーで即返り、**毎回 板は生存** |
| `ai_call` | 参照出力と一致、**0.417 秒** |
| 旧方言（回帰） | `42` |
| `/smp-bench?n=200000`（回帰） | 4 コア 38ms → 12ms、3.16 倍 |
| `/actor/load`（Mac 側 `--xinu-jit` の C。回帰） | `tick 1` `tick 2` |

**正典ガイド 5/10 が、実機で無加工のまま動く。** 残る 5 本はこの装置に無い機能。

### 状態
- `cc/abcl2c.c`（+163/-7）と `system/tcp_server.c`（+22/-12）は**未コミット**。
- **Mac 側 `--xinu-jit` の黙った欠落（`reply` の消失、`new` の引数と `init` の消失）は
  まだ直っていない。** 機内経路は直ったので、当面は `POST /cc` に AIPL を投げるのが確実。

---

## 2026-09-02（続き4）— Mac 側の黙った欠落も直した。両方コミット済み

| コミット | |
|---|---|
| `xinu-rpi5` `4e4ec74` | 機内 abcl2c の追従 ＋ `/cc` の振り分け（焼き済み = `8773feb9…`） |
| `abclcp-project` `c734a1c` | Mac 側 `--xinu-jit` の黙った欠落 |

### Mac 側で直した3つ（すべて実機で壊れ方を確認してから直した）
1. **`reply(e)` が `/* unsupported call */` に潰れていた** → `return e;`
2. **`new C(args)` が引数と `init` を捨てていた** → `__new_init()` 経由で `init` を呼ぶ
3. **トップレベル `var` が `main` のローカル** → 大域として先に宣言（g3・g6 が
   `cc: undefined variable` になっていた）

### exit 0 の意味を変えた
落とすものは**名前つきでコメントに出す**（`/* unsupported call: web_listen */`）。
さらに `aipl2c` は落とし物があれば一覧を stderr に出して **exit 4**。
**`exit 0` = 何も消えていない**、を意味するようにした。

### いまの到達点

| 経路 | 正典ガイド |
|---|---|
| 機内 `POST /cc`（AIPL を無加工で） | **5/10**。残りは `future`/`select`/`web_listen`/`wait`/`timeout` を名前つきで断る |
| Mac 側 `--xinu-jit` → `POST /actor/load` | 翻訳できる 8 本中 **7 本が落とし物なし**（g5 のみ `web_*` を自己申告して exit 4）。実機で g1 g2 g3 g4 g6 g8 g9 が期待どおり |
| 前段の構文（`--check`） | 7/10。**g7・g10 の後置 `timeout` はまだ構文が無い**（AST・infer・c_translator に波及するので未着手） |

### 残っていること
- **g7 / g10 の後置 `now x.m() timeout N else E`。** 前段に構文が無い。
  受理して「このバックエンドは未対応」と明示的に落とすのが次の一手。
- **g3 は型検査を通らない**（`--no-typecheck` が要る）。正典の `infer.ml` は 2197 行、
  この実装は 997 行で別物。移植が要る。
- 機内側は `reply` 以外の並行機能（`future`/`select`/`timeout`）を持たない。

---

## 2026-09-03 — 後置の期限を通した。`--check` 7/10 → 9/10

`abclcp-project` `e5505ab`。

- `now t.m(a) timeout <ms> else <式>` / `await f timeout <ms> else <式>` を前段に追加。
  AST に `Deadline (body, ms, else)`。型は本体の型で、else 節をそれに合わせる。
- **単項マイナスの文法規則が無かった**（`-1` がどこにも書けなかった）。`0 - e` に落とす。
- **OCaml ランタイムでは期限を実装した。** `Condition` に時限待ちが無いので
  `wait_reply_slot_timeout`（少し寝ながらスロットを覗く）を書いた。期限を過ぎたら
  スロットを片づけてから else 節を評価する（遅れて来た返信で誰かが起きないように）。
- **Xinu バックエンドは期限を守れない。** `cc_select` は -1 を返すだけの空実装、
  `recvtime` も空（`system/xinu_compat.c:297`）。黙って無視すると期限切れのときに
  else 節ではなく本体の値を返すので、**名前を出して落とす（exit 4）**。

### いまの数字

| | |
|---|---|
| `--check` | **9/10**（残る g3 は型検査の差。前段の問題ではない） |
| `--xinu-jit` | **10 本すべて翻訳される**。7 本は落とし物なし、3 本（g5・g7・g10）は自己申告して exit 4 |
| 機内 `POST /cc` | 5/10（`future`/`select`/`timeout`/`web`/`wait` を名前つきで断る） |

実機で g10 が `fork = 1` `latch = 2`（同名フィールドが別物であることの確認）。
g1 g2 g3 g4 g6 g8 g9 も回帰なし。

### 途中で分かった、直していないこと
- **トップレベルの `if … else` は前から通らない**（メソッド本体の中なら通る）。
  基準版でも同じなので、今回の変更による回帰ではない。
- **`repl_thread -f` はガイドを走らせられない**（`Actor g not found`）。これも前から。
  期限の実機確認は Xinu 側で行った。
- 文法の衝突が 0 → 4 shift/reduce。ocamlyacc は shift を選び期待どおりだが、
  数が増えたことは覚えておく。

### 残っている最後の一つ
**g3 の型検査。** 正典の `infer.ml` は 2197 行、この実装は 997 行で別物。
`--no-typecheck` なら通る。ここを詰めるなら移植が要る。

---

## 2026-09-03（続き）— g3 も通った。**`--check` 10/10。ここが到達点**

`abclcp-project` `b3ecf79`。**移植は要らなかった。多重定義の登録順の問題だった。**

`add_mono` は PREPEND するので、あとに登録したものほど先に試される。算術は
`(float,float) → (int,int) → (int,float) → (float,int)` の順に登録されており、
先に試されるのは `(float,int)`。`n - k`（n:int フィールド、k:未注釈の仮引数）では
第1引数で落ち、次の `(int,float)` が **k を float に釘付けして成立**してしまう。
結果 `n - k : float` となり、int のフィールドへの代入で mismatch。

**関係演算子（`typing_env.ml` 2.5.2）には同じ修正が既に入っていて、コメントにも
理由が書いてあった。算術にだけ入っていなかった。**

両辺とも未知のときに `(int,int)` が先に当たるようになったので、`sqrt(int)` を
断らないよう数学関数に int 版を足した（`abs` は int→int）。

### 回帰の測り方（712 本のコーパス）
`~/ocaml-app/abclcp-project` と `~/aios/abclcp` の **`.aipl`/`.abcl` 全 712 本**を
`--check` に通して、変更前後で判定が変わったものを数えた。

| | |
|---|---|
| 変更前 | 437 / 712 が通る |
| 変更後 | **454 / 712**（+17）。**通らなくなったものは 0** |

一度 1 本だけ落ちた（`feature_a_hm/sample3_rat_real.aipl`）。原因は
`sqrt(int)` で、数学関数に int 版を足して解消。**コーパスを測らなければ
見逃していた。**

### 最終的な数字

| | |
|---|---|
| `--check` | **10 / 10** |
| `--xinu-jit` | 10 本すべて翻訳。7 本は落とし物なし、3 本（g5・g7・g10）は自己申告して exit 4 |
| 機内 `POST /cc`（AIPL を無加工で） | 5 / 10 |
| 実機 `/actor/load`（**型検査つきで生成**） | g1・g3・g6・g8・g9 が期待どおり |

### このセッションのコミット
```
xinu-rpi5        4e4ec74  機内 abcl2c の追従 ＋ /cc の振り分け（焼き済み 8773feb9）
abclcp-project   c734a1c  --xinu-jit の黙った欠落（reply / new の引数 / 大域変数）
abclcp-project   e5505ab  後置の期限 timeout … else … と単項マイナス
abclcp-project   b3ecf79  算術の多重定義の順序（g3）
```

---

## 2026-09-03（続き2）— 期限を実装、文字列の化けを修正。**残りはカーネルの仕事**

| コミット | |
|---|---|
| `xinu-rpi5` `dd0e30d` | 機内でも後置の期限（ガイド 5/10 → 6/10）**★まだ焼いていない** |
| `abclcp-project` `d5dc50f` | Mac 側でも後置の期限（落とし物なしが 8/10 に） |
| `abclcp-project` `81826d7` | 非 ASCII の文字列リテラルが数字の羅列になっていた |

### 期限は `cc_now_ms` と `dispatch` だけで書けた（カーネル変更なし）
生成 C の中に受け皿を出す:
```c
int __dl_call(int to,int meth,int a0,int a1,int a2,int a3,int ms,int elsev){
  int t0; int r;
  t0 = v_int_of(cc_now_ms());
  r = dispatch(to, meth, a0, a1, a2, a3);
  if (v_int_of(cc_now_ms()) - t0 > ms) { return elsev; }
  return r; }
```
**返る値は正典と同じだが、呼び出しは中断されない。** 協調ポンプなので割り込めない。
実機で `now s.quick(7) timeout 1000 else -99` → `7`、`now s.heavy(7) timeout 1 else -99` → `-99`。
**機内 cc は仮引数 8 個まで**（9 個で `cc: too many parameters (max 8)`。実機で確認）。

### 非 ASCII の文字列が壊れていた（Mac 側だけ）
`String.escaped` が `"あ"` を `"\227\129\130"` にする。**機内 cc の `read_escape`
（`cc/parse.c:30`）は数値エスケープを持たない**（`default: r = c;`）ので `\2` が `'2'` になり、
数字がそのまま文字列に入っていた。コンパイルも実行も通り exit 0 のまま壊れる。
xinu-jit 専用の出し方に変え、0x80 以上は生の UTF-8 で出す。
**機内の abcl2c は元から正しかった。**

### ★ ここから先はカーネルの仕事（実測で分かったこと）
`/cc` は**割り込みを塞いだ RX 処理の中**で走り、**`CC_TIMEOUT_MS = 250`** で打ち切られる
（`cc/cc.c:123`）。実機に 250ms 超の計算を投げると
`cc: aborted (ran past the runaway-loop deadline)` が返る（板は無事）。

- **g7** … `wait(300)` がこの枠の外。原理的に動かない。
- **g2（future/await）・g4（select）** … アクターごとに待てる実行体（スタック／スレッド）が要る。
  いまは単一の FIFO をその場で流し切る協調ポンプ。`cc_select` は `-1` を返すだけの空実装
  （`cc/cc.c:613`）、`recvtime` も空（`system/xinu_compat.c:297`）。
- **g5** … `web_expose` / `web_listen`。Pi3 にはある（`/api/x/<path>`）が Pi5 には無い。
  `system/tcp_server.c` に動的ルート表と `cc_web_expose` の extern を足す話。

**いずれも「JIT を ISR の外へ出す」実行モデルの変更が前提**で、これまでの各修正とは規模が違う。

### 焼き待ち
`compile/kernel_2712.img` = **`f246e8f2…`**（2,024,584 B）がビルド済み・未焼き。
退避は `compile/kernel_2712.img.pre-deadline-8773feb9…`。
焼くと機内 `POST /cc` が 5/10 → 6/10 になる。**焼かなくても Mac 側経路は今の板で動く。**

---

## 2026-09-03 — `f246e8f2…` を焼いて確認。**機内 6/10。ここが現在の到達点**

カード上の退避（新しい順）:
`…bak-pre-deadline-8773feb9…` / `…bak-pre-ccdetect-edb1a254…` / `…bak-pre-a2c-syntax-dc700cf9…`
`config.txt` は `388a3a1a…` のまま無傷。

### 実機で通ったこと（すべて 192.168.3.101、板は最後まで生存）

| 確認 | 結果 |
|---|---|
| **正典 AIPL を無加工で `POST /cc`** | **6本**（g1 g3 g6 g8 g9 ＋ **g10**）が期待どおり |
| **期限が効く** | `now s.quick(7) timeout 1000 else -99` → `7`／`now s.heavy(7) timeout 1 else -99` → **`-99`** |
| 通らない4本 | `future` / `select` / `web_listen` / `wait` と名前つきで断る |
| 以前 板を止めていた入力8種 | 全部が即返り、毎回 板は生存（`b2` は期限対応で通るようになった＝正しい変化） |
| `ai_call` | 参照出力と一致、0.481 秒 |
| 旧方言／`/smp-bench`／`/actor/load` | 回帰なし（`42` ／ 2.92倍 ／ `tick 1` `tick 2`） |

### 最終的な数字

| 経路 | 正典ガイド |
|---|---|
| Mac 側 `--check` | **10 / 10** |
| Mac 側 `--xinu-jit`（落とし物なし） | **8 / 10**（g5 は `web_*`、g7 は `wait(300)` と await の期限） |
| 機内 `POST /cc`（AIPL を無加工で） | **6 / 10** |

### このセッションのコミット
```
xinu-rpi5        4e4ec74  機内 abcl2c を正典の表層構文へ（0/10 → 5/10）＋ /cc の振り分け
xinu-rpi5        dd0e30d  機内でも後置の期限（5/10 → 6/10）★焼き済み = f246e8f2
abclcp-project   c734a1c  --xinu-jit の黙った欠落（reply / new の引数 / 大域変数）
abclcp-project   e5505ab  後置の期限と単項マイナス（--check 7/10 → 9/10）
abclcp-project   b3ecf79  算術の多重定義の順序（--check 9/10 → 10/10）
abclcp-project   d5dc50f  now の期限を事後判定で実装（落とし物なし 8/10）
abclcp-project   81826d7  非 ASCII の文字列リテラルが数字の羅列になっていた
```

### 次にやるなら（実行モデルの変更が前提）
`/cc` は割り込みを塞いだ RX 処理の中で走り `CC_TIMEOUT_MS=250` で打ち切られる。
**JIT を ISR の外（自前のスレッド）へ出す**のが、g2（future/await）・g4（select）・
g7（`wait(300)`）に共通の前提。g5（`web_expose`）だけは独立で、
`system/tcp_server.c` に動的ルート表と extern を足す話。

---

## 2026-09-03 — `b2a215fb…` を焼いて確認。**機内 7/10・Mac 側 9/10。残りは g7 だけ**

カード上の退避（新しい順）: `…bak-pre-webexpose-f246e8f2…` /
`…bak-pre-deadline-8773feb9…` / `…bak-pre-ccdetect-edb1a254…`。`config.txt` 無傷。

### g5（web_expose）が実機で完動した

```
POST /cc            <- g5_web.aipl をそのまま（機内翻訳器）
GET  /api/x/        -> exposed routes: /api/x/echo -> actor #0
GET  /api/x/echo?method=say&args=hi   -> => echo: hi
GET  /api/x/echo?method=note&args=test -> [note] test
GET  /api/x/nope?...                  -> no such exposed path: /nope
```
`/actor/load` 経由でも同じ。**公開ルートはプログラムと寿命を共にする**
（次のプログラムを載せると消える。設計どおり）。

**ポート指定は無視する。** 板の HTTP は 80 番で、`web_listen` の意図（境界を開く）は
既に満たされている。板は番号を記録するだけ。

### `/cc` を常駐させた
web_expose したアクターへ後から到達するために、`/cc` も `/actor/load` と同じく
dispatch/`__method_id` を保持し `g_res_loaded` を立てる。アリーナは静的バッファで
`arena_init` が bump ポインタを戻すだけなので、解放をやめてもリークしない。
**`/cc` を連続で投げると前のアクター世界が置き換わる**形になる（回帰は出ていない）。

### 字句解析器のバグも直した
**文字列リテラルを 62 バイトで打ち切り、閉じ引用符まで読み進めずに戻っていた。**
日本語は 1 文字 3 バイトなので簡単に超え、その先を文字列の中身から解析し始めて
まったく無関係な場所で `expected expression` になっていた（g5 がこれ）。
長さを 160 バイトにし、溢れても閉じ引用符までは読み進め、溢れたら名前をつけて断る。

### 実機で通ったこと（板は最後まで生存）
正典 AIPL を無加工で `POST /cc` → **7本**（g1 g3 **g5** g6 g8 g9 g10）。
通らない3本は `future` / `select` / `wait` と名前つきで断る。
以前 板を止めていた入力8種は全部が即返り、毎回 生存。
`ai_call` 0.425 秒（参照出力と一致）、旧方言 `42`、`/smp-bench` 2.92倍、
`/actor/load` 回帰なし。

### 最終的な数字

| 経路 | セッション開始時 | 現在 |
|---|---|---|
| Mac 側 `--check` | 7 / 10 | **10 / 10** |
| Mac 側 `--xinu-jit`（落とし物なし） | 実質 0 | **9 / 10** |
| 機内 `POST /cc` | 0 / 10 | **7 / 10** |

**残るは g7 だけ。** `wait(300)` が JIT の打ち切り（`CC_TIMEOUT_MS=250`、割り込みを
塞いだ RX 処理の中で走る）を超えるので、この装置では原理的に動かない。
g2（future/await）・g4（select）と併せて、**JIT を ISR の外へ出す**実行モデルの
変更が前提になる。

### このセッションのコミット
```
xinu-rpi5        4e4ec74  機内 abcl2c を正典の表層構文へ（0/10 → 5/10）＋ /cc の振り分け
xinu-rpi5        dd0e30d  機内でも後置の期限（5/10 → 6/10）
xinu-rpi5        7fbf773  web_expose（6/10 → 7/10）＋ /cc 常駐化 ＋ 文字列リテラル修正
abclcp-project   c734a1c  --xinu-jit の黙った欠落（reply / new の引数 / 大域変数）
abclcp-project   e5505ab  後置の期限と単項マイナス（--check 7/10 → 9/10）
abclcp-project   b3ecf79  算術の多重定義の順序（--check 9/10 → 10/10）
abclcp-project   d5dc50f  now の期限を事後判定で実装
abclcp-project   81826d7  非 ASCII の文字列リテラルが数字の羅列になっていた
abclcp-project   ba3ab29  web_expose / web_listen（落とし物なし 8/10 → 9/10）
```

---

## ★ 2026-09-03 — SD を抜き差ししなくてもカーネルを載せられる（chainload）

**`/chainload` が生きていた。** ルート一覧には出ていない隠し口。RAM 上だけで完結するので、
**失敗しても電源を入れ直せば SD の版に戻る**（SD には一切書かない）。実際に成功した。

```
# 1) 8KB ずつに割る
split -b 8192 -a 4 -d compile/kernel_2712.img /tmp/chunks/c
# 2) 順に送る（off は 8192 の倍数。off=0 でステージングが作り直される）
curl --data-binary @/tmp/chunks/c0000 "http://192.168.3.101/chainload?off=0"
  -> ok off=0 n=8192 total=8192
# 3) 総量を確かめる
curl "http://192.168.3.101/chainload"      -> staged=2028696 bytes
# 4) 起動（応答は返らない。curl は timeout で抜ける）
curl -m 8 "http://192.168.3.101/chainload?go=1&len=2028696"
# 5) 5〜10 秒で ping が戻る
```

台本: **`~/projects/xinu-rpi5/tools_chainload_upload.sh`**（`SC_DIR` にチャンク置き場を渡す）。
2.03MB / 248 チャンクで、**間隔を空けずに送っても取りこぼさなかった**（1分ほど）。
ステージングは 4MB（`CHAIN_STAGE_CAP`）。

### ★★ 訂正（同日、実測後）— **chainload は検証環境として使えない**

上の手順で「起動する」ところまでは本当に動く。しかし **載った先の板は別物になる**:

| | SD 起動 | chainload 起動 |
|---|---|---|
| `cores_online` | **4** | **1** |
| `/smp-bench` 200000 | 38ms → 13ms（2.92倍） | 38ms → 38ms（**1.00倍**） |
| シェルの `cc`（`/run?cmd=`） | 動く | **40秒で応答なし** |
| 安定性 | 安定 | **数回リクエストすると ping ごと落ちる** |

**副コアが起動しない。** 元の起動で上がった副コアは、chainload した
カーネルには引き継がれない（あるいは古いカーネルのコードを踏んだまま）。
そのため SMP もスケジューラ周りも本来の姿ではなく、**ここで測った回帰は
コードの良し悪しを表さない**。

→ **カーネル変更の検証は、これまでどおり SD に焼いて行う。** chainload は
「新しいイメージが起動可能か」を見るだけの用途に留める。
（この節は最初「試作は chainload で」と書いたが、実測して取り消した。
  書いてから測ったのは順序が逆だった。）

※ Pi3 では net kexec が brick した記録があるが、**Pi5 のこの経路は通った**。
   コード中のコメントによれば、以前は固定番地 0x4000000 を使って自分の .bss（約69MB）を
   踏んでいたのを kmalloc からの確保に直してある。実行前に「載せ先が動作中のカーネルと
   重なっていないか」も検査する。

---

## 2026-09-03 — future/await を実装したが**実機で板が固まる**。焼いてあるが使うな

**焼いてあるのは `0602c4e0…`（future 入り）。commit `84259c5`。**
戻す先はカード上の `kernel_2712.img.bak-pre-future-83c90942…`（機内 7/10 の安定版）。

### 通ったこと
翻訳器は 9/10 になった（g2 と g7 が加わり、残るは g4=select だけ）。
生成 C も正典どおりの形（`cc_future` → `cc_await_dl`）。ファズ 4,000 件 crash=0 hang=0。
**`future` を使わない既存機能は実機で全部無傷**:
機内7本・`/api/x/`・`ai_call`・`/actor/load`・`wait(300)` 0.346秒・`/smp-bench` 4コア2.92倍。

### 固まったこと
**`future` を実際に使うと板が固まる**（g2 をシェル経路で走らせ、45秒無応答 → ping 断）。
電源再投入で復旧する。**入れた歯止め（時間打ち切り・`proc_is_free` によるスロット再利用の
確認）は効かなかった** ＝ 時間切れに達する前に、より低い層で止まっている。

### 手当てが足りていないところ（次の一手はここ）
値ヒープ（`vheap_alloc`/`lheap_alloc`）は `irq_save` で守ったが、**プロセスをまたいで
共有のグローバルが他にもある**。future プロセスと await 側が同時に触る:

| | 何が起きうるか |
|---|---|
| `g_deadline` / `g_aborted` | `cc_tick()` の打ち切り判定を両者が共有。片方が `cc_set_deadline()` すると他方の予算が飛ぶ |
| `g_cap` / `g_caplen` / `g_capcap` | 出力捕捉バッファ。両者が書くと壊れる |
| `cc_pump()` / `g_mq` / `g_qh` / `g_qt` | 単一 FIFO。future 側の `dispatch` が `cc_pump` を回すと、待っている側と奪い合う |
| `g_sel[]`, `g_nact` ほか | 同様に共有 |

**値ヒープだけ守っても足りなかった、というのが今回の学び。**
やるなら「どのグローバルをプロセスごとに分けるか」を先に洗い出す設計が要る。

### 今日スケジューラ周りで2回板を固めた
1回目: シェルの `cc` が `proc_resched()` 1回で完了と見なしていた（`wait` で破れた）→ 修正済み
2回目: 今回の `future`
**どちらも「実機で試すのが最初の検証」になっていた。** ホストでスケジューラを再現できない
以上、ここは設計レビューを厚くするしかない。

---

## 2026-09-03 セッション終了時の状態

**カードに焼いてあるのは `83c90942…`（`future` 非搭載の安定版）。** 実機で全項目確認済み:
機内7本（g1 g3 g5 g6 g8 g9 g10）、通らない3本は名前つきで断る、`/api/x/echo` → `echo: hi`、
`ai_call` 参照出力一致、シェル `wait(300)` → `work(21) = 42`、`/smp-bench` 4コア3.0倍。

**`future` 版 `0602c4e0…` はカードから外した。** 手元の `compile/kernel_2712.img` と
commit `84259c5` に残っている。**焼くと `future` を使った瞬間に板が固まる。**

### 到達点

| 経路 | セッション開始時 | 終了時 |
|---|---|---|
| Mac 側 `--check` | 7 / 10 | **10 / 10** |
| Mac 側 `--xinu-jit`（落とし物なし） | 実質 0 | **9 / 10** |
| 機内 `POST /cc`（AIPL を無加工で） | **0 / 10** | **7 / 10** |

### このセッションのコミット（12件）
```
xinu-rpi5        4e4ec74  機内 abcl2c を正典の表層構文へ（0/10 → 5/10）＋ /cc の振り分け
xinu-rpi5        dd0e30d  機内でも後置の期限（5/10 → 6/10）
xinu-rpi5        7fbf773  web_expose（6/10 → 7/10）＋ /cc 常駐化 ＋ 文字列リテラル修正
xinu-rpi5        6e34c48  打ち切り予算を経路ごとに、wait(ms) を通す
xinu-rpi5        84259c5  future/await を別プロセスで（★実機で板が固まる。焼くな）
abclcp-project   c734a1c  --xinu-jit の黙った欠落（reply / new の引数 / 大域変数）
abclcp-project   e5505ab  後置の期限と単項マイナス（--check 7/10 → 9/10）
abclcp-project   b3ecf79  算術の多重定義の順序（--check 9/10 → 10/10）
abclcp-project   d5dc50f  now の期限を事後判定で実装
abclcp-project   81826d7  非 ASCII の文字列リテラルが数字の羅列になっていた
abclcp-project   ba3ab29  web_expose / web_listen
abclcp-project   638f03e  wait(ms) を cc_wait に出す
```

### 潰した「黙って壊れる」系 6 件
`reply` の消失／`new` の引数と `init` の消失／トップレベル変数の未宣言／
`new <知らないクラス>` の領域外参照（実機を落とす）／非 ASCII 文字列の数字化け／
文字列リテラル62バイトでの字句同期崩れ。
`aipl2c` は落とし物があれば **exit 4** で終わる＝ exit 0 が「何も消えていない」を意味する。

### 次回の一手
1. **g4（select）と future の並行化** — まず「どの共有グローバルをプロセスごとに分けるか」の
   洗い出しから（`g_deadline` / `g_cap` / `cc_pump`+FIFO / `g_sel` / `g_nact`）。
   → [[feedback-xinu-shared-globals-across-procs]]
2. 未コミットの `wait` 可視化（`cc/cc.c`、インライン経路で `wait` を断る）。
   **`0602c4e0` に含まれているので、安定版へ入れ直すなら再ビルドが要る。**

---

## 2026-09-03（最終）— Pi 3 と機能を揃えた。**前段 10/10・実機で 8/10 が正典どおり**

**焼いてあるのは `e7e468d3…`。** commit `738a8b6`。
カード上の退避は `…bak-pre-sel-83c90942…`（ひとつ前の安定版）。

### Pi 3 との差は `select` の待ち合わせだけになった

| | Pi 3 | Pi 5 |
|---|---|---|
| `future` / `await` | 継続分割（並行ではない） | **即時実行**（並行ではない） |
| `select` | 継続分割で**本当に待てる** | **待たない**（FIFO を覗くだけ） |
| 正典ガイド | 10/10 `accepted=1`（出力確認は g5 のみ） | **10/10 走る・8/10 が正典どおり** |
| 連続投入 | **8 秒空けないと HTTP が詰まる**（4 秒だと 6 本で死ぬ） | 3 秒間隔で終日安定 |

### 実機で測った結果（`POST /cc`）
g1 g2 g3 g5 g6 g8 g9 g10 は正典どおり。残り2本は一致しない:

- **g7** … `slow(timed out) = 42`（正典は 0）。`/cc` では `wait(300)` が使えず
  期限を超えないため。**シェル経路（`/run?cmd=cc /home/g7.abcl`）なら 0 で一致**（実測）。
- **g4** … `timed out`（正典は `via select:1`）。`select` が待たないため。
  **経路を変えても同じ。ここだけ Pi 3 が上。**

### 実装（スケジューラにもプロセスにも触っていない）
`future` はその場で `dispatch` して値をスロット（8本）へ。`await` はスロットを読むだけ。
`select` は FIFO を走査し、一致したメッセージを取り出して `g_sel` に置き、FIFO から詰めて除く。
**先にプロセス方式（`84259c5`）で本物の並行を試して板を固めたので、撤去した。**

### 残った一手
**Pi 5 にも継続分割を実装すれば `select` が待てる**（Pi 3 で実績のある方式。
`select` の位置でメソッドを二つに割り、局所変数を隠しフィールドへ退避）。
スケジューラには触らずに済むが、前段の作り替えとしては小さくない。

### ガイドを公開した
`~/aios/abclcp/docs/aipl_guide_{ja,en}.tex` の第39章を全面改訂（日 44 頁／英 39 頁）。
`https://kodamay.org/reports/2026-09-03_aipl_guide.pdf` に公開済み（要認証・md5 一致）。
一覧の最新版を 08-23 版から差し替え（全84本のまま）。site commit `087cd96`。
