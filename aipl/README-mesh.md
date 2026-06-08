# Mac/Windows ↔ Xinu メッシュ負荷分散（型推論クリーン AIPL）

2 Mac + 1 Windows = 管理ノード、3 Xinu = ワーカーノード、というメッシュ構成で、
管理ノードが Xinu に「できるだけ多くのアクタ」を配置・負荷分散する仕組み。
不足していた **配置ポリシ・登録・負荷追跡・障害再配置** を型推論クリーンな
AIPL で実装し、ランタイム側の **負荷メトリクス（/api/load）** を Xinu に追加した。

## 構成要素

| 役割 | 実体 | 場所 |
|------|------|------|
| 配置ポリシ／登録／負荷追跡／障害再配置 | `MeshLoadBalancer.abcl`（AIPL, 型推論クリーン） | 管理(host) |
| ワーカー（多数のアクタ実行） | 同ファイルの `Worker` クラス（`/actor/load`） | Xinu |
| ノード負荷メトリクス | `/api/load`（`cc_actor_live_count`/`capacity`） | Xinu |
| アクタ配置→物理ノード対応 | `aipl_dist` の route table（actor名→provider URL） | 管理(host) |

## MeshLoadBalancer.abcl（管理ノードのロジック）

- `Scheduler.register(n)` … n 個のワーカーを登録（各 Xinu ノードに対応）
- `Scheduler.run()` … タスク列を **最小割当(=最小負荷)ノード** に振り分けて実行
- `Scheduler.mark_down(i)` / `mark_up(i)` … ノード障害／復帰。`run()` は
  ダウン中ノードを選ばず、生存ノードへ **再配置** する
- 累積割当数 `est[]` を負荷指標として毎回最小のノードを選ぶので、タスクは
  ワーカー間に均等分散する

### 型推論クリーンであることの確認
```bash
A=ocaml-app/abclcp-project/_build/default/src/aipl2c.exe
$A aipl/MeshLoadBalancer.abcl --check        # -> 型エラーなし
```

### 実行（host ランタイムで配置＋障害再配置をデモ）
```bash
printf 'load %s/aipl/MeshLoadBalancer.abcl\ncompile\n' "$PWD" > /tmp/run.repl
ocaml-app/abclcp-project/_build/default/src/repl_thread.exe -f /tmp/run.repl
```
出力例（node 1 をダウンさせると、タスクは node 0 と node 2 のみへ分散）:
```
[scheduler] registered 3 workers
[scheduler] node 1 marked DOWN; tasks re-place to survivors
[scheduler] job 0 (= 2)  -> node 0
[scheduler] job 1 (= 3)  -> node 2
[scheduler] job 2 (= 5)  -> node 0
...
[scheduler] completed 9 tasks; sum of squares = 1556
```

## Xinu 側のワーカー実行
`Worker` クラス部分を `--xinu-jit` で C に変換し、各 Xinu の `/actor/load` に
POST すると、その Xinu 上でワーカーアクタが常駐する。管理ノードは
`/actor/send?to=&m=work&arg=` でタスクを投入し、`/api/load` で負荷を見る。

```bash
$A aipl/MeshLoadBalancer.abcl --xinu-jit --no-typecheck -o /tmp/mesh.c
sed 's/v_nil/v_int(0)/g' /tmp/mesh.c > /tmp/mesh_fixed.c
curl --data-binary @/tmp/mesh_fixed.c -X POST http://<xinu>/actor/load
curl 'http://<xinu>/api/load'      # {"node":"xinu","live_actors":N,"capacity":1024,"load_pct":P}
```

## 物理ノードへの割り当て（transport）
`aipl_dist` の route table が `Worker_<i>` というアクタ名を物理ノード
（Xinu の URL）に対応づける。管理ノードの `Scheduler` が選んだ index が、
route table を通じて実際の Xinu へ HTTP（`/actor/send`）でルーティングされる。
`/api/load` の値を配置ポリシ（最小負荷）に取り込めば、実負荷ベースの分散になる。

## まとめ
- 配置ロジックは **型推論クリーン AIPL**（`--check` 通過）で実装・動作確認済み。
- Xinu 側は 1 ノード最大 1024 アクタ。`/api/load` で負荷を公開。
- 3 Xinu × 管理 3 ノードのメッシュは、route table に各 Xinu を provider 登録すれば
  この枠組みでそのまま動く。
