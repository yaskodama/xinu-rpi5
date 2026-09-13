#!/usr/bin/env bash
# measure.sh —— AIPL のアクター並列を粒度別に測る（1 ロード = 数ラウンド）。
#
#   VM の tick はロード時に一度だけ投入され、Collector の wait(1) が次のラウンドを
#   起こす。そこで「計器を 0 にする → 標本を読み込む → 一定時間待つ → 読む」を
#   1 標本とし、us と msgs から **1 メッセージあたりの費用** を出す。
#
#   使い方: ./measure.sh <host> <ラベル> [待ち秒]   → TSV を stdout へ
#   例:     ./measure.sh 192.168.3.101 workers 4 > workers.tsv
set -u
HOST="${1:-192.168.3.101}"
LABEL="${2:-run}"
WAIT="${3:-4}"
GRAN="1 10 50 200 1000 5000 20000 100000 200000"
REPS="${REPS:-2}"

printf "# host=%s label=%s wait=%ss reps=%s\n" "$HOST" "$LABEL" "$WAIT" "$REPS"
printf "# %s\n" "$(curl -s -m 10 "http://$HOST/version" | head -1)"
printf "# %s\n" "$(curl -s -m 10 "http://$HOST/smpmode" | head -1)"
printf "granularity\trep\tus\tmsgs\tbatches\tus_per_msg\tpath\n"

for g in $GRAN; do
  avm="/tmp/AiplBenchG$g.avm"
  [ -f "$avm" ] || continue
  len=$(stat -f%z "$avm" 2>/dev/null || stat -c%s "$avm")
  for r in $(seq 1 "$REPS"); do
    curl -s -m 10 "http://$HOST/avm-par?on=1&reset=1" >/dev/null
    curl -s -m 20 -X POST --data-binary @"$avm" "http://$HOST/actor/loadvm?off=0" >/dev/null
    curl -s -m 20 "http://$HOST/actor/loadvm?go=1&len=$len" >/dev/null
    sleep "$WAIT"
    out="$(curl -s -m 10 "http://$HOST/avm-par")"
    us=$(echo "$out"  | sed -n 's/.* us=\([0-9]*\).*/\1/p')
    ms=$(echo "$out"  | sed -n 's/.* msgs=\([0-9]*\).*/\1/p')
    nb=$(echo "$out"  | sed -n 's/.*nbatch=\([0-9]*\).*/\1/p')
    pa=$(echo "$out"  | sed -n 's/.*path=\([a-z]*\).*/\1/p')
    if [ -n "${ms:-}" ] && [ "${ms:-0}" -gt 0 ]; then
      upm=$(python3 -c "print(f'{$us/$ms:.3f}')")
    else
      upm="NA"
    fi
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$g" "$r" "${us:-NA}" "${ms:-NA}" "${nb:-NA}" "$upm" "${pa:-NA}"
  done
done
