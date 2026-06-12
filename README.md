# xinu-rpi5

Embedded Xinu port for the **Raspberry Pi 5 (BCM2712, Cortex-A76, AArch64)**,
running bare-metal on real hardware.

![Xinu running on a Raspberry Pi 5](docs/boot-screen.jpg)

*Xinu on real Pi 5 hardware: a window-managed HDMI desktop with a live shell,
system-status / actors / VFS-tree / memory panels, an on-screen keyboard, and a
spinning 3-D wireframe wine glass — driven by a USB mouse and keyboard over the
RP1 xHCI controllers.*

Bootstrapped from [`yaskodama/xinu-rpi`](https://github.com/yaskodama/xinu-rpi)
(32-bit arm-qemu / arm-rpi) and the AArch64 boot pattern from
[`radlyeel/leex`](https://github.com/radlyeel/leex). It has since grown from a
serial hello-world into a small interactive system: an HDMI window manager,
USB-A input, wired + WiFi networking with an HTTP gateway, microSD storage, an
on-device C/AIPL compiler, and a `kexec` selector across four OS variants.

## What works

A single-image AArch64 kernel with the MMU enabled (BCM2712, Cortex-A76):

- **HDMI framebuffer + window system** — a desktop with a live shell window,
  system-status / actors / VFS-tree / memory panels, an on-screen keyboard, and
  a spinning 3-D wireframe.
- **USB mouse + keyboard** — via the two RP1 DWC3/xHCI controllers (over PCIe);
  the cursor and shell input are USB-driven.
- **Wired Ethernet over RP1** — TCP/IP + an HTTP gateway on static
  `192.168.3.101` (`/run?cmd=…`, `/fs`, `/microsd/write`, `/api/actors`, …).
- **WiFi (CYW43455)** — scan / WPA2 / DHCP / ping; connects on `wifi on` (no
  auto-connect at boot).
- **microSD (FAT32)** — read + persistent small-file write, mounted at
  `/microsd`.
- **`kexec` selector** — swap to another kernel in RAM with no re-flash and no
  brick risk; **four OS variants** (Full / OS1-minimal / OS2-no-WM /
  OS3-lower-res) from one source tree.
- **On-device tooling** — `cc` / `make` compile and run a C/AIPL program in
  place; an AIPL actor gateway.

**Boot media.** The board **boots from a USB stick**: the firmware loads
`kernel_2712.img` from the stick's FAT partition (`bootfs`). The on-board
microSD slot is normally empty and is used as a **data** area (mounted at
`/microsd`).

## Key specifications

| Item | Value |
|------|-------|
| SoC | BCM2712 (Cortex-A76, AArch64) |
| Kernel image | `kernel_2712.img` (entry `0x80000`) |
| MMU | enabled (identity map, caches off) |
| Debug UART | `0x107D001000` (3-pin JST, 115200 8N1) |
| HDMI | 1920×1080×32 default (OS3 = 1280×720) |
| Wired IP (static) | `192.168.3.101` |
| USB host | RP1 DWC3/xHCI (over PCIe) |

## Build

```sh
# Mac (Homebrew AArch64 cross toolchain — pick either):
brew install aarch64-elf-gcc           # GNU
brew install --cask gcc-arm-embedded   # ARM-supplied

cd compile
make pi5        # Full OS         -> kernel_2712.img
make pi5-osmin  # OS1 (minimal)   -> kernel_min.img
make pi5-os2    # OS2 (no WM)      -> kernel_os2.img
make pi5-os3    # OS3 (lower res)  -> kernel_os3.img
```

## Deploy

Write the image to the **USB stick** (FAT partition `bootfs`), then boot the Pi:

```sh
diskutil mount /dev/disk4s1
cp compile/kernel_2712.img /Volumes/bootfs/kernel_2712.img
sync && diskutil unmount /dev/disk4s1
# put the stick back in the Pi 5 and power on (boot log appears after ~10 s)
```

The microSD card may hold the alternate OS images (`OS1.IMG`, `OS2.IMG`,
`OS3.IMG`) for `kexec`.

## The four OS variants + kexec

| Name | Image | Characteristics |
|------|-------|-----------------|
| Full OS | `kernel_2712.img` | window system + networking (all features) |
| OS1 | `OS1.IMG` | shell only — no networking, no windows |
| OS2 | `OS2.IMG` | all features, text console instead of the WM |
| OS3 | `OS3.IMG` | all features, HDMI lowered to 1280×720 |

`kexec` loads a kernel from the microSD into RAM and jumps to it without a power
cycle — RAM-only, so a bad image is recovered by power-cycling back into the USB
full OS (no brick risk). Only bare-metal Xinu images (< 4 MB) can be chainloaded.

```sh
kexec /microsd/OS1.IMG       # boot OS1 (minimal)
kexec /microsd/OS2.IMG       # boot OS2 (no WM)
```

## Remote operation (HTTP)

The Full OS / OS2 / OS3 serve HTTP on `192.168.3.101`:

```sh
curl 'http://192.168.3.101/run?cmd=ls%20/microsd'
curl 'http://192.168.3.101/run?cmd=wifi%20status'
curl -X POST --data-binary "hello" http://192.168.3.101/microsd/write/TEST.TXT
```

Endpoints: `/run?cmd=`, `/fs` (+ `/fs/ls`, `/fs/cat`, `/fs/write`),
`/microsd/write/<NAME>`, `/usb/...`, `/api/actors`, `/send?to=&m=`.

## WiFi

```sh
wifi on        # connect (embedded credentials; no auto-connect at boot)
wifi status    # SSID + IP
wifi scan      # nearby APs
wifi off
```

WiFi credentials are embedded at build time from a `.gitignore`d file — never
commit the password.

## Documentation

- **User's manual** (operator-facing, EN + JA): typeset PDFs under `docs/`
  (`docs/xinu-pi5-manual-en.pdf` / `docs/xinu-pi5-manual.pdf`; sources
  `docs/xinu-pi5-manual-en.tex` / `docs/xinu-pi5-manual.tex`). Markdown mirrors:
  `USERS_MANUAL_EN.md` / `USERS_MANUAL_JA.md`. They cover flashing, the OS
  variants, `kexec`, microSD, the shell, remote HTTP operation, WiFi, and USB.
- Session handoff notes: `NEXT_SESSION.md`.

## License

Inherits from upstream Xinu / leex (BSD-style). See `LICENSE` once the
source-of-truth license file is added.
