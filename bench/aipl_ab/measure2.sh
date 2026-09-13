#!/usr/bin/env bash
# measure2.sh —— 実験2(アクター数)と実験3(不揃い)を測る。1 ロード＝数ラウンド。
set -u
HOST="${1:-192.168.3.101}"; WAIT="${2:-4}"; REPS="${REPS:-2}"
printf "# %s\n" "$(curl -s -m 10 "http://$HOST/version" | head -1)"
printf "# %s\n" "$(curl -s -m 10 "http://$HOST/smpmode" | head -1)"
printf "sample\trep\tus\tmsgs\tbatches\tlastbn\tus_per_msg\tpath\n"
for s in AiplN8 AiplN16 AiplN32 AiplUneven; do
  avm="/tmp/$s.avm"; [ -f "$avm" ] || continue
  len=$(stat -f%z "$avm" 2>/dev/null || stat -c%s "$avm")
  for r in $(seq 1 "$REPS"); do
    curl -s -m 10 "http://$HOST/avm-par?on=1&reset=1" >/dev/null
    curl -s -m 20 -X POST --data-binary @"$avm" "http://$HOST/actor/loadvm?off=0" >/dev/null
    curl -s -m 20 "http://$HOST/actor/loadvm?go=1&len=$len" >/dev/null
    sleep "$WAIT"
    o="$(curl -s -m 10 "http://$HOST/avm-par")"
    us=$(echo "$o"|sed -n 's/.* us=\([0-9]*\).*/\1/p'); ms=$(echo "$o"|sed -n 's/.* msgs=\([0-9]*\).*/\1/p')
    nb=$(echo "$o"|sed -n 's/.*nbatch=\([0-9]*\).*/\1/p'); lb=$(echo "$o"|sed -n 's/.*lastbn=\([0-9]*\).*/\1/p')
    pa=$(echo "$o"|sed -n 's/.*path=\([a-z]*\).*/\1/p')
    if [ "${ms:-0}" -gt 0 ] 2>/dev/null; then u=$(python3 -c "print(f'{$us/$ms:.3f}')"); else u=NA; fi
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$s" "$r" "${us:-NA}" "${ms:-NA}" "${nb:-NA}" "${lb:-NA}" "$u" "${pa:-NA}"
  done
done
