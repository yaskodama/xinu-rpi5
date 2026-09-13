#!/bin/bash
B=http://192.168.3.101
D=$SC_DIR/chunks
fail=0; last=""
for f in $D/c*; do
  i=$(basename $f); i=${i#c}
  off=$((10#$i * 8192))
  r=$(curl -s -m 20 --data-binary @$f "$B/chainload?off=$off")
  case "$r" in
    ok*) last="$r" ;;
    *)   echo "★ off=$off で失敗: $r"; fail=$((fail+1)); [ $fail -ge 3 ] && { echo "3回失敗したので中止"; exit 1; } ;;
  esac
  case $((10#$i % 40)) in 0) echo "  ... $last" ;; esac
done
echo "最後: $last"
echo "=== 板が持っている総量 ==="; curl -s -m 15 "$B/chainload"; echo
