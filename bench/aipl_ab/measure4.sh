#!/usr/bin/env bash
# measure4.sh —— 実験1（核0の参加）と、core_hits つきの P 系列/不揃い標本の再測。
#
#   実験1 は「核0 が対称スケジューラに参加したか」を **数** で見る:
#     /smpmode  の poll_calls= / poll_hits=   … 核0 が引き取りに来た回数 / 取れた回数
#     /smpsched の ran_per_core=a/b/c/d       … 先頭(核0)が 0 でなくなるか
#     /avm-par  の core_hits=a/b/c/d          … AIPL メッセージをコア別に何通実行したか
#
#   使い方: ./measure4.sh <host> <ラベル> [待ち秒]
set -u
HOST="${1:-192.168.3.101}"; LABEL="${2:-run}"; W="${3:-6}"
HERE="$(cd "$(dirname "$0")" && pwd)"
AVMC="$HOME/projects/aice-avm/_build/default/compile_avm.exe"
g() { curl -s -m 240 "http://$HOST$1"; }

echo "# $(g /version | head -1) / $(g /smpmode | head -1)"
echo "# 核0の参加: $(g /smpmode | tr '\n' ' ' | sed -n 's/.*\(poll_calls=[0-9]* poll_hits=[0-9]*\).*/\1/p')"

echo
echo "== N-Queens n=13 (対称スケジューラ側) =="
printf "  %-8s %8s %10s %s\n" tasks ms solutions ran_per_core
for t in 4 8 13; do
  o="$(g "/smpsched?n=13&tasks=$t" | tr '\n' ' ')"
  printf "  %-8s %8s %10s %s\n" "$t" \
    "$(sed -n 's/.* ms=\([0-9]*\).*/\1/p' <<<"$o")" \
    "$(sed -n 's/.*solutions=\([0-9]*\).*/\1/p' <<<"$o")" \
    "$(sed -n 's/.*ran_per_core=\([0-9/]*\).*/\1/p' <<<"$o")"
done
echo "  核0の参加: $(g /smpmode | tr '\n' ' ' | sed -n 's/.*\(poll_calls=[0-9]* poll_hits=[0-9]*\).*/\1/p')"

echo
echo "== AIPL 標本（core_hits つき）=="
printf "  %-12s %10s %6s %8s %12s %s\n" 標本 us msgs batches us/msg core_hits
for b in AiplP2 AiplP4 AiplP8 AiplP16 AiplP32 AiplUneven; do
  src="$HERE/$b.abcl"; avm="/tmp/$b.avm"
  "$AVMC" "$src" "$avm" >/dev/null 2>&1 || { echo "  $b: コンパイル失敗"; continue; }
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
  upm="NA"; [ "${ms:-0}" -gt 0 ] && upm=$(python3 -c "print(f'{$us/$ms:.2f}')")
  printf "  %-12s %10s %6s %8s %12s %s\n" "$b" "${us:-?}" "${ms:-?}" "${nb:-?}" "$upm" "${ch:-?}"
done
