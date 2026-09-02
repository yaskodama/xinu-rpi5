# 次回の再開メモ — Xinu Pi3 / Pi5 の AIPL 実行系

最終更新: 2026-08-31

## 0. まずこれをやる

**2026-08-31 の終了時点で、Pi3 も Pi5 も電源が入っていない（両方とも ping 無応答）。**
まず電源を入れる。Pi5 は直前に固まっていたので、入れ直しになる。
そのあと下の「未完了 A」から。

```
ping 192.168.3.101            # Pi5
curl -s http://192.168.3.101/ # ルート一覧が返れば生きている
curl -s http://192.168.3.50:8080/api/routes   # Pi3（ポート 8080）
```

## 1. 今どうなっているか

| | Pi 3 | Pi 5 |
|---|---|---|
| IP / ポート | `192.168.3.50` : **8080** | `192.168.3.101` : **80** |
| リポジトリ | `~/projects/xinu-rpi3` (`arm-rpi3-port`) | `~/projects/xinu-rpi5` (`manet-m14-udp`) |
| 実行方式 | `.avm` をカーネル内 VM が解釈 | AIPL→C→機内 `cc` が AArch64 に JIT |
| 前段 | Mac の `compile_avm`（`~/projects/aice-avm`） | **機内**の `cc/abcl2c.c` |
| 投入 | `POST /actor/loadvm?ask=0` | `POST /cc`（本文が `class` で始まれば AIPL） |
| 値を読む | `web_expose` + `/api/x/<path>?method=&args=` | `/cc` の応答にそのまま出る |
| 焼いてある | `01e5f3e2…`（2,161,816 B） | `dc700cf9…`（2,020,488 B） |
| 手元のビルド | 同上 | **`870ea069…`（未焼き）** |

**`ai_call` は両機で動いている。** 同じモデル（Karpathy `stories260K`, 1.06 MB を
`.rodata` に焼き込み）で、生成文は Pi3・Pi5・Mac 参照実装で **72 バイト一致**。
速さだけが違う（Pi5 は 0.42〜0.64 秒、Pi3 は起動直後 2〜6 秒）。

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
