#!/usr/bin/env bash
# run_aipl_ab.sh —— AIPL のアクター並列を「ワーカーズ型」と「対称SMP型」で測る。
#
#   同じ AIPL 標本(AiplBenchG<粒度>.abcl)を、同じカーネル・同じ起動のまま
#   両方式で走らせ、**1 メッセージあたりの配布+実行コスト**を比べる。
#   粒度を振って走らせるので、方式が逆転する点（交差点）も出る。
#
# 前提: 板が **workers モードで起動している**こと（/smpmode?on=1 は一方通行なので、
#       ワーカーズ型を先に全部測ってから切り替える）。
#
#   使い方: ./run_aipl_ab.sh [host] [各標本の測定秒数]
set -u
HOST="${1:-192.168.3.101}"
SECS="${2:-6}"
HERE="$(cd "$(dirname "$0")" && pwd)"
AVMC="$HOME/projects/aice-avm/_build/default/compile_avm.exe"
GRAN="200 1000 5000 20000 100000 200000"

say() { printf '%s\n' "$*"; }
get() { curl -s -m 20 "http://$HOST$1"; }

load_sample() {   # $1 = .abcl
  local src="$1" b avm len
  b="$(basename "$src" .abcl)"; avm="/tmp/$b.avm"
  "$AVMC" "$src" "$avm" >/dev/null 2>&1 || { say "  コンパイル失敗: $src"; return 1; }
  len=$(stat -f%z "$avm" 2>/dev/null || stat -c%s "$avm")
  curl -s -m 20 -X POST --data-binary @"$avm" "http://$HOST/actor/loadvm?off=0" >/dev/null
  curl -s -m 20 "http://$HOST/actor/loadvm?go=1&len=$len" >/dev/null
  return 0
}

measure() {       # $1 = 表示名  -> "us msgs nbatch us_per_msg_x100"
  get "/avm-par?on=1&reset=1" >/dev/null
  sleep "$SECS"
  get "/avm-par"
}

mode_now() { get /avm-par | sed 's/.*path=//'; }

say "=== 計器 ==="
say "  $(get /version | head -1)"
say "  $(get /smpmode | head -1)"
say "  $(get /smplock?n=20000 | head -1)"

for phase in workers symmetric; do
  if [ "$phase" = symmetric ]; then
    say ""
    say "=== 対称SMP型へ切り替え（一方通行）==="
    say "  $(get '/smpmode?on=1' | head -1)"
    sleep 2
  fi
  say ""
  say "=== AIPL アクター並列 / $phase ($SECS 秒ずつ) ==="
  printf "  %-10s %10s %8s %8s %14s\n" 粒度 us msgs batches us/msg
  for g in $GRAN; do
    load_sample "$HERE/AiplBenchG$g.abcl" || continue
    sleep 1
    out="$(measure)"
    us=$(echo "$out"   | sed -n 's/.* us=\([0-9-]*\).*/\1/p')
    ms=$(echo "$out"   | sed -n 's/.* msgs=\([0-9-]*\).*/\1/p')
    nb=$(echo "$out"   | sed -n 's/.* nbatch=\([0-9-]*\).*/\1/p')
    pm=$(echo "$out"   | sed -n 's/.* us_per_msg_x100=\([0-9-]*\).*/\1/p')
    printf "  %-10s %10s %8s %8s %11s.%02d us\n" "$g" "${us:-?}" "${ms:-?}" "${nb:-?}" \
           "$(( ${pm:-0} / 100 ))" "$(( ${pm:-0} % 100 ))"
  done
done

say ""
say "※ us = 並列配布に費やした総時間、msgs = そのあいだに配ったメッセージ数。"
say "   us/msg が小さいほど、その方式は AIPL のメッセージを安く配れている。"
say "   粒度を下げるほど配布そのものの費用が支配的になり、方式の差が開く。"
