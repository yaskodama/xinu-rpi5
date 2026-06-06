#!/usr/bin/env python3
# chainload.py — push a new kernel to a running Pi 5 over HTTP and boot it,
# with no SD/USB reflash and no power cycle.  Requires the running kernel to
# have the /chainload endpoint (system/kexec.c + loader/chainload.S).
#
#   python3 chainload.py <ip> [kernel_2712.img]
#
# Stages the image in chunks at RAM 0x4000000, then GET /chainload?go=1 makes
# the box relocate a trampoline and jump into the freshly-uploaded kernel.

import sys, time, urllib.request

ip  = sys.argv[1] if len(sys.argv) > 1 else "192.168.3.213"
img = sys.argv[2] if len(sys.argv) > 2 else "kernel_2712.img"
CHUNK = 8192

data = open(img, "rb").read()
n = len(data)
print(f"chainload {img} ({n} bytes) -> {ip}")

for off in range(0, n, CHUNK):
    chunk = data[off:off+CHUNK]
    req = urllib.request.Request(f"http://{ip}/chainload?off={off}",
                                 data=chunk, method="POST")
    r = urllib.request.urlopen(req, timeout=15).read().decode().strip()
    print(f"  {off+len(chunk):>7}/{n}  {r}")

print("GO -> jumping into the new kernel (connection will drop)")
try:
    urllib.request.urlopen(f"http://{ip}/chainload?go=1&len={n}", timeout=4).read()
except Exception:
    pass   # the box jumps away mid-response; a dropped connection means success
print("done — the Pi 5 is booting the uploaded kernel (~5-10 s)")
