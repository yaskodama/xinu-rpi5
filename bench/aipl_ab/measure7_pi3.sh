#!/usr/bin/env bash
# measure7_pi3.sh —— Pi 3 用。載せ方(POST ?ask=0)と計器名が違うだけで作法は measure7 と同じ。
#   載せる → 暖機して捨てる → 計器を戻す → 測る。
#   Pi 3 の us は重なりで過大になるので使わない。主役は ok（検算に通ったラウンド数）。
set -u
HOST="$1"; WARM="$2"; MS="$3"; shift 3
HERE="$(cd "$(dirname "$0")" && pwd)"
AVMC="$HOME/projects/aice-avm/_build/default/compile_avm.exe"
g() { curl -s -m 240 "http://$HOST$1"; }
printf "%-14s %6s %4s %11s %10s %s\n" 標本 ok ng round/s ms/round core_hits
for b in "$@"; do
  src="$HERE/$b.abcl"; avm="/tmp/$b.avm"
  "$AVMC" "$src" "$avm" >/dev/null 2>&1 || { echo "$b: コンパイル失敗"; continue; }
  curl -s -m 90 -X POST --data-binary @"$avm" "http://$HOST/actor/loadvm?ask=0" >/dev/null
  g "/avm-run?ms=$WARM" >/dev/null            # 暖機（捨てる）
  g "/avm-par?reset=1" >/dev/null
  g "/avm-run?ms=$MS" >/dev/null
  o="$(g /avm-par | tr '\n' ' ')"
  ok=$(sed -n 's/.* ok=\([0-9]*\).*/\1/p' <<<"$o"); ng=$(sed -n 's/.* ng=\([0-9]*\).*/\1/p' <<<"$o")
  ch=$(sed -n 's/.*core_hits=\([0-9\/]*\).*/\1/p' <<<"$o")
  rs="NA"; mr="NA"
  [ "${ok:-0}" -gt 0 ] && { rs=$(python3 -c "print(f'{$ok*1000/$MS:.1f}')"); mr=$(python3 -c "print(f'{$MS/$ok:.1f}')"); }
  printf "%-14s %6s %4s %11s %10s %s\n" "$b" "${ok:-?}" "${ng:-?}" "$rs" "$mr" "${ch:-?}"
done
