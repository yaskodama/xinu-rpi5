# 次回の再開メモ — Xinu Pi3 / Pi5 の AIPL 実行系

最終更新: 2026-08-31

## 0. まずこれをやる

**Pi5 が固まっている。電源を入れ直す。** そのあと下の「未完了 A」から。

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
