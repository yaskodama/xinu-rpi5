#!/usr/bin/env bash
# measure7.sh —— 暖機を捨てて定常値を測る（AIPL 用の決定版）。
#
#   これまでの測り方には 2 つの穴があった:
#     (1) 駆動源が無く、実際には読み込み直後の 2 ラウンドしか走っていなかった
#     (2) その 2 ラウンドは **冷間始動の過渡** で、定常値の約 1/2 の時間しかかからない
#   ここでは 載せる → 暖機して捨てる → 計器を戻す → 測る、の順にする。
#   /bench(N-Queens) では最初からこの作法だったのに、AIPL 側だけ抜けていた。
#
#   使い方: ./measure7.sh <host> <暖機ms> <測定ms> <標本名...>
set -u
HOST="$1"; WARM="$2"; MS="$3"; shift 3
HERE="$(cd "$(dirname "$0")" && pwd)"
AVMC="$HOME/projects/aice-avm/_build/default/compile_avm.exe"
g() { curl -s -m 240 "http://$HOST$1"; }
echo "# $(g /version | head -1) / $(g /smpmode | head -1) / 暖機${WARM}ms 測定${MS}ms"
printf "%-14s %5s %4s %11s %11s %8s %s\n" 標本 ok ng ms/batch ms/round nbatch core_hits
for b in "$@"; do
  src="$HERE/$b.abcl"; avm="/tmp/$b.avm"
  "$AVMC" "$src" "$avm" >/dev/null 2>&1 || { echo "$b: コンパイル失敗"; continue; }
  len=$(stat -f%z "$avm")
  curl -s -m 30 -X POST --data-binary @"$avm" "http://$HOST/actor/loadvm?off=0" >/dev/null
  g "/actor/loadvm?go=1&len=$len" >/dev/null
  g "/avm-par?on=1" >/dev/null
  g "/avm-run?ms=$WARM" >/dev/null          # 暖機（捨てる）
  g "/avm-par?reset=1" >/dev/null
  g "/avm-run?ms=$MS" >/dev/null
  o="$(g /avm-par | tr '\n' ' ')"
  ok=$(sed -n 's/.* ok=\([0-9]*\).*/\1/p' <<<"$o"); ng=$(sed -n 's/.* ng=\([0-9]*\).*/\1/p' <<<"$o")
  us=$(sed -n 's/.* us=\([0-9]*\).*/\1/p' <<<"$o"); nb=$(sed -n 's/.*nbatch=\([0-9]*\).*/\1/p' <<<"$o")
  ch=$(sed -n 's/.*core_hits=\([0-9\/]*\).*/\1/p' <<<"$o")
  mb="NA"; [ "${nb:-0}" -gt 0 ] && mb=$(python3 -c "print(f'{$us/$nb/1000:.1f}')")
  mr="NA"; [ "${ok:-0}" -gt 0 ] && mr=$(python3 -c "print(f'{$MS/$ok:.1f}')")
  printf "%-14s %5s %4s %11s %11s %8s %s\n" "$b" "${ok:-?}" "${ng:-?}" "$mb" "$mr" "${nb:-?}" "${ch:-?}"
done
