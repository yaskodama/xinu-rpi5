#!/bin/bash
# tools/chainload.sh — push a kernel image to a running Pi 5 Xinu over HTTP and
# warm-boot it, WITHOUT an SD swap or power cycle.  Uses the kernel's built-in
# /chainload route (system/tcp_server.c): POST the image in chunks to staging
# RAM at 0x4000000, then GET ?go=1 to relocate + jump (loader/chainload.S).
#
#   usage: tools/chainload.sh [image] [host]
#     image  kernel image to send   (default: compile/kernel_2712.img)
#     host   Pi 5 IP                 (default: 192.168.3.101)
#
# Requires the Pi to be on the network (Ethernet link up / same LAN as host).
# A bad image only wedges RAM — a real power-cycle recovers (nothing is written
# to the SD card).
set -euo pipefail

IMG="${1:-compile/kernel_2712.img}"
HOST="${2:-192.168.3.101}"
CHUNK=4096                                   # route accepts <= 8 KiB/POST; stay safe

[ -f "$IMG" ] || { echo "no such image: $IMG" >&2; exit 1; }
SIZE=$(stat -f%z "$IMG" 2>/dev/null || stat -c%s "$IMG")   # macOS / Linux stat

echo ">>> chainload '$IMG' ($SIZE bytes) -> http://$HOST"
if ! curl -sf -m 4 "http://$HOST/chainload" >/dev/null; then
    echo "!! Pi not reachable at $HOST (Ethernet link down? wrong IP?)" >&2
    exit 1
fi

blk=0
off=0
while [ "$off" -lt "$SIZE" ]; do
    dd if="$IMG" bs="$CHUNK" skip="$blk" count=1 2>/dev/null | \
        curl -sf --data-binary @- "http://$HOST/chainload?off=$off" >/dev/null
    blk=$((blk + 1))
    off=$((blk * CHUNK))
    printf "\r    uploaded %d / %d bytes" "$( [ "$off" -lt "$SIZE" ] && echo "$off" || echo "$SIZE" )" "$SIZE"
done
echo

echo ">>> booting staged image ($SIZE bytes) ..."
# This request never gets a clean reply — the kernel jumps before responding.
curl -s -m 3 "http://$HOST/chainload?go=1&len=$SIZE" || true
echo ">>> sent go. Watch the serial console for the new kernel banner."
