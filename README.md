# xinu-rpi5

Embedded Xinu port for the Raspberry Pi 5 (BCM2712, Cortex-A76,
AArch64-only).

This is a brand-new repository, bootstrapped from
[`yaskodama/xinu-rpi`](https://github.com/yaskodama/xinu-rpi)
(arm-qemu / arm-rpi platforms, 32-bit) and the AArch64 boot pattern
from [`radlyeel/leex`](https://github.com/radlyeel/leex).  The
existing 32-bit Xinu tree stays where it is; Pi 5's mandatory
AArch64 instruction set, new MMIO layout, and RP1 I/O hub make a
clean split easier than ifdef-walling everything in place.

## Status

| Phase (from `AIPL_XinuRPi5_Round1.aice`) | State |
|------------------------------------------|-------|
| **B0** aarch64 toolchain ready           | ⏳ user-side (`brew install aarch64-none-elf-gcc`) |
| **B1** AArch64 boot stub (`kernel/boot.S`)| ✅ |
| **B2** `kernel_2712.img` build pipeline   | ✅ |
| **U0** PL011 UART0 driver                  | ✅ |
| **U1** kprintf — banner only for now       | ✅ (basic puts; full kprintf later) |
| M0 MMU flat identity map                  | ⏳ |
| M1 Kernel heap                            | ⏳ |
| S0 AArch64 context switch                 | ⏳ |
| S1 GIC-400 + generic timer                | ⏳ |
| **X0** xsh on Pi 5 — interactive REPL      | ✅ minimal (help/echo/hello/mem/peek/uptime/reboot) |
| X1 AIPL hello                             | ⏳ |

The boot path right now is **leex stub → BSS clear → `kernel_main` →
banner → `shell_main()` REPL**.  USB-serial cable on header pins 8 / 10
should show:

```
================================================
  Xinu Pi5 hello (AArch64, BCM2712, kernel_2712.img)
  PL011 UART0 @ 0x107D001000, 115200 8N1
  bootstrap: leex-style stub + xinu-rpi5 main
================================================

Round 1 phase B/U done — entering interactive shell.
Next milestones: M0 MMU, S0 ctxsw, S1 GIC+timer

type `help` for the command list.
xinu-pi5$ _
```

The shell handles backspace/DEL, echoes input, and dispatches via a
`davidxyz/xinuPi`-style `centry` table.  Built-ins:

| command           | what it does                                        |
|-------------------|-----------------------------------------------------|
| `help` / `?`      | list registered commands                            |
| `echo <words…>`   | echo args back (whitespace-collapsed)               |
| `hello`           | friendly greeting (smoke marker)                    |
| `mem`             | `__bss_start`, `__bss_end`, `_end` from `link.ld`   |
| `peek <hex_addr>` | read 32-bit MMIO word; e.g. `peek 0x107d001018` (UART_FR) |
| `uptime`          | raw `CNTPCT_EL0` (until phase S1 wires the timer)   |
| `reboot`          | stub — spins until power-cycle (RP1 watchdog TBD)   |

## Hardware vs Pi 4 (leex baseline)

|              | Pi 4 (leex baseline) | **Pi 5 (this repo)** |
|--------------|----------------------|----------------------|
| SoC          | BCM2711              | **BCM2712**          |
| Cores        | Cortex-A72 ×4        | **Cortex-A76 ×4**    |
| MMIO base    | 0xFE000000           | **0x107C000000**     |
| I/O hub      | direct on SoC        | **RP1 (PCIe 別チップ)** |
| Firmware img | `kernel8.img`        | **`kernel_2712.img`**|
| UART base    | `0xFE201000`         | **`0x107D001000`**   |

## Build

```sh
# Mac (Homebrew toolchain):
brew install --cask gcc-arm-embedded   # ships aarch64-none-elf-gcc
cd kernel
make                                    # → kernel/kernel_2712.img
```

Override the toolchain location if you installed it elsewhere:

```sh
make GCCPATH=$HOME/aarch64/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-elf
```

## Install

Insert a Pi-5 SD card with the FAT32 bootfs partition mounted.  The
canonical Mac path is `/Volumes/bootfs`:

```sh
cd kernel
make install SDCARD=/Volumes        # copies kernel_2712.img + config.txt
```

The `sdcard/` directory at the repo root holds the canonical
`config.txt`.  It sets `arm_64bit=1`, `kernel=kernel_2712.img`,
`enable_uart=1`, and `dtparam=uart0=on` so the firmware locks
UART0 to GPIO14/15 at the fixed 48 MHz reference clock.

You also need the regular Pi-5 firmware blobs on the same partition
(`bootcode.bin`, `start4.elf` etc).  The easiest way is to format the
card with a stock Raspberry Pi OS image and then overwrite
`kernel_2712.img` + `config.txt`.

## Run

### Real Pi 5 hardware

1. Wire a 3.3 V USB-serial adapter to header pins 8 (TXD → GPIO14),
   10 (RXD → GPIO15) and a GND pin (e.g. 6).
2. On the host: `screen /dev/tty.usbserial-XXXX 115200`
3. Power-cycle the Pi.  The banner above appears within ~5 seconds.

### QEMU `virt` (no Pi 5 machine in upstream QEMU yet)

QEMU 11 doesn't model BCM2712 / RP1, so we cross-build a tiny variant
that swaps the PL011 base (`0x107D001000` → `0x09000000`) and the
load address (`0x80000` → `0x40080000`).  Source is shared; only
`-DUART0_BASE=...` and `link_virt.ld` differ.

```sh
cd kernel
make qemu          # interactive  — Ctrl-A X to quit
make qemu-smoke    # pipes a canned command list, writes qemu-smoke.log
```

Recorded `make qemu-smoke` output (`qemu-system-aarch64 11.0,
-cpu cortex-a76`):

```
xinu-pi5$ hello
hello from Xinu on Raspberry Pi 5 (BCM2712, AArch64)
xinu-pi5$ mem
__bss_start = 0x0000000040080db0
__bss_end   = 0x0000000040080eb0
_end        = 0x0000000040080eb0
xinu-pi5$ peek 0x09000018          # PL011 FR
[0x0000000009000018] = 0x00000000000000c0   # TXFE | RI
xinu-pi5$ uptime
cntpct_el0 = 0x0000000003bf91e7
xinu-pi5$ echo smoke ok
smoke ok
```

## Layout

```
xinu-rpi5/
├── kernel/
│   ├── boot.S          # AArch64 entry stub (leex pattern, Pi-5 tuned)
│   ├── link.ld         # load address 0x80000 (Pi 5 firmware)
│   ├── link_virt.ld    # load address 0x40080000 (QEMU virt)
│   ├── Makefile        # aarch64-elf → kernel_2712.img + kernel_virt.img
│   ├── main.c          # banner + shell handoff
│   ├── shell.c         # bare-metal REPL (davidxyz/xinuPi-style)
│   ├── shell.h
│   ├── uart.c          # PL011 UART0 @ 0x107D001000 (+ getc/getline)
│   └── uart.h
├── sdcard/
│   └── config.txt      # firmware settings (arm_64bit=1, etc)
└── README.md
```

## Roadmap (`AIPL_XinuRPi5_Round1.aice`)

The full Round 1 plan lives in the companion abclcp-project repo
at `aice-pi-evolution/experiments/2026-05-22_xinu_rpi5/`.  Twelve
phases across six directions:

| Direction | Phases | One-liner |
|-----------|--------|-----------|
| **B** Boot | B0–B2 | toolchain, AArch64 stub, kernel_2712.img |
| **U** UART | U0–U1 | PL011 UART0, kprintf |
| **M** Memory | M0–M1 | MMU flat ID map, freelist heap |
| **S** Scheduler | S0–S1 | AArch64 ctxsw, GIC + generic timer |
| **X** Userland | X0–X1 | xsh on Pi 5, AIPL hello |
| **N** Network (stretch) | N0–N1 | RP1 discover, VideoCore VII framebuffer |

The legacy 32-bit Xinu (`yaskodama/xinu-rpi`) is the regression
anchor — none of its smokes are allowed to break while this repo
catches up.

## License

Inherits from upstream Xinu / leex (BSD-style).  See LICENSE once
the source-of-truth license file is added.
