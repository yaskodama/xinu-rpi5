#!/usr/bin/env bash
# measure3.sh — 実験2(バッチ長=アクター数) と 実験3(不揃い) を測る
set -u
HOST="${1:-192.168.3.101}"; WAIT="${2:-5}"
printf "# %s / %s\n" "$(curl -s -m 10 http://$HOST/version|head -1)" "$(curl -s -m 10 http://$HOST/smpmode|head -1)"
printf "sample\tus\tmsgs\tbatches\tlastbn\tus_per_msg\tpath\n"
for s in AiplP2 AiplP4 AiplP8 AiplP16 AiplP32 AiplUneven; do
  avm="/tmp/$s.avm"; [ -f "$avm" ] || continue
  len=$(stat -f%z "$avm")
  curl -s -m 10 "http://$HOST/avm-par?on=1&reset=1" >/dev/null
  curl -s -m 20 -X POST --data-binary @"$avm" "http://$HOST/actor/loadvm?off=0" >/dev/null
  curl -s -m 20 "http://$HOST/actor/loadvm?go=1&len=$len" >/dev/null
  sleep "$WAIT"
  o="$(curl -s -m 10 "http://$HOST/avm-par")"
  us=$(echo "$o"|sed -n 's/.* us=\([0-9]*\).*/\1/p'); ms=$(echo "$o"|sed -n 's/.* msgs=\([0-9]*\).*/\1/p')
  nb=$(echo "$o"|sed -n 's/.*nbatch=\([0-9]*\).*/\1/p'); lb=$(echo "$o"|sed -n 's/.*lastbn=\([0-9]*\).*/\1/p')
  pa=$(echo "$o"|sed -n 's/.*path=\([a-z]*\).*/\1/p')
  [ "${ms:-0}" -gt 0 ] 2>/dev/null && u=$(python3 -c "print(f'{$us/$ms:.2f}')") || u=NA
  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$s" "${us:-NA}" "${ms:-NA}" "${nb:-NA}" "${lb:-NA}" "$u" "${pa:-NA}"
done
