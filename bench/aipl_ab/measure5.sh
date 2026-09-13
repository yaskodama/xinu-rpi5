#!/usr/bin/env bash
# measure5.sh —— 標本を並べて「1 バッチの makespan」と core_hits を測る。
#
#   AIPL のバッチは 1 tick で全ワーカが同時に発射されるので、
#   us / batches = **1 バッチの makespan**（＝そのバッチを配り終えるまでの壁時計）。
#   実験A/B/C はいずれもこの値を予測値と突き合わせるので、us/msg ではなく
#   makespan を主役にする。
#
#   使い方: ./measure5.sh <host> <待ち秒> <標本名...>
set -u
HOST="$1"; W="$2"; shift 2
HERE="$(cd "$(dirname "$0")" && pwd)"
AVMC="$HOME/projects/aice-avm/_build/default/compile_avm.exe"
g() { curl -s -m 240 "http://$HOST$1"; }

echo "# $(g /version | head -1) / $(g /smpmode | head -1)"
printf "%-14s %10s %6s %8s %12s %s\n" 標本 us msgs batches makespan_ms core_hits
for b in "$@"; do
  src="$HERE/$b.abcl"; avm="/tmp/$b.avm"
  "$AVMC" "$src" "$avm" >/dev/null 2>&1 || { echo "$b: コンパイル失敗"; continue; }
  len=$(stat -f%z "$avm")
  g "/avm-par?on=1&reset=1" >/dev/null
  curl -s -m 30 -X POST --data-binary @"$avm" "http://$HOST/actor/loadvm?off=0" >/dev/null
  g "/actor/loadvm?go=1&len=$len" >/dev/null
  sleep "$W"
  o="$(g /avm-par | tr '\n' ' ')"
  us=$(sed -n 's/.* us=\([0-9]*\).*/\1/p' <<<"$o")
  ms=$(sed -n 's/.* msgs=\([0-9]*\).*/\1/p' <<<"$o")
  nb=$(sed -n 's/.*nbatch=\([0-9]*\).*/\1/p' <<<"$o")
  ch=$(sed -n 's/.*core_hits=\([0-9/]*\).*/\1/p' <<<"$o")
  mk="NA"; [ "${nb:-0}" -gt 0 ] && mk=$(python3 -c "print(f'{$us/$nb/1000:.1f}')")
  printf "%-14s %10s %6s %8s %12s %s\n" "$b" "${us:-?}" "${ms:-?}" "${nb:-?}" "$mk" "${ch:-?}"
done
