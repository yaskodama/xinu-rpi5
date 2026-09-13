#!/usr/bin/env bash
# measure6.sh —— 駆動源(/avm-run)つきの測定。標本数が桁違いに増え、正誤も ng で分かる。
#
#   これまでの measure5.sh は「載せて W 秒待つ」だったが、この板は HDMI が無く
#   avm_tick を呼ぶ経路が存在しないため、実際には読み込み直後の 2 ラウンドしか
#   走っていなかった。/avm-run?ms= が待ち行列に tick を積み直しながら回す。
#
#   主役の量は **1 ラウンドの所要時間 = 窓 / ok**。ok は検算に通ったラウンド数なので、
#   ng=0 であることが同時に「その測定が正しい実行の上のものだ」という保証になる。
#
#   使い方: ./measure6.sh <host> <窓ms> <標本名...>
set -u
HOST="$1"; MS="$2"; shift 2
HERE="$(cd "$(dirname "$0")" && pwd)"
AVMC="$HOME/projects/aice-avm/_build/default/compile_avm.exe"
g() { curl -s -m 240 "http://$HOST$1"; }
echo "# $(g /version | head -1) / $(g /smpmode | head -1) / 窓 ${MS}ms"
printf "%-14s %6s %4s %10s %8s %7s %11s %s\n" 標本 ok ng ms/round us msgs nbatch core_hits
for b in "$@"; do
  src="$HERE/$b.abcl"; avm="/tmp/$b.avm"
  "$AVMC" "$src" "$avm" >/dev/null 2>&1 || { echo "$b: コンパイル失敗"; continue; }
  len=$(stat -f%z "$avm")
  g "/avm-par?on=1&reset=1" >/dev/null
  curl -s -m 30 -X POST --data-binary @"$avm" "http://$HOST/actor/loadvm?off=0" >/dev/null
  g "/actor/loadvm?go=1&len=$len" >/dev/null
  g "/avm-run?ms=$MS" >/dev/null
  o="$(g /avm-par | tr '\n' ' ')"
  ok=$(sed -n 's/.* ok=\([0-9]*\).*/\1/p' <<<"$o"); ng=$(sed -n 's/.* ng=\([0-9]*\).*/\1/p' <<<"$o")
  us=$(sed -n 's/.* us=\([0-9]*\).*/\1/p' <<<"$o"); ms=$(sed -n 's/.* msgs=\([0-9]*\).*/\1/p' <<<"$o")
  nb=$(sed -n 's/.*nbatch=\([0-9]*\).*/\1/p' <<<"$o"); ch=$(sed -n 's/.*core_hits=\([0-9\/]*\).*/\1/p' <<<"$o")
  mr="NA"; [ "${ok:-0}" -gt 0 ] && mr=$(python3 -c "print(f'{$MS/$ok:.3f}')")
  printf "%-14s %6s %4s %10s %8s %7s %11s %s\n" "$b" "${ok:-?}" "${ng:-?}" "$mr" "${us:-?}" "${ms:-?}" "${nb:-?}" "${ch:-?}"
done
