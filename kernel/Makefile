# kernel/Makefile — build kernel_2712.img for Raspberry Pi 5.
#
# Phase B0/B1/B2 of AIPL_XinuRPi5_Round1.aice.  Produces a flat binary
# image at the load address declared in link.ld (0x80000) using the
# aarch64-none-elf toolchain.
#
# Conventions inherited from leex (github.com/radlyeel/leex):
#   - GCCPATH points at the toolchain root (override on the make
#     command line if your install lives elsewhere)
#   - `make install` copies the produced image to $(SDCARD)/bootfs
#     and unmounts the card so it's safe to pull
# Pi-5-specific tweaks:
#   - Image name is kernel_2712.img (Pi 5 firmware ignores
#     kernel8.img / kernel7l.img etc.)
#   - -mcpu=cortex-a76 + -mgeneral-regs-only (no SIMD/FP, no extra
#     state for ctxsw to save in Round 1)

CFILES   = $(wildcard *.c)
OFILES   = $(CFILES:.c=.o)

# Toolchain — Homebrew installs either `aarch64-elf-gcc` (gcc-aarch64-elf
# formula) or `aarch64-none-elf-gcc` (arm-toolchain cask); auto-detect.
GCCPATH ?= /opt/homebrew
CROSS   ?= $(shell test -x $(GCCPATH)/bin/aarch64-elf-gcc && echo aarch64-elf- || echo aarch64-none-elf-)
CC       = $(GCCPATH)/bin/$(CROSS)gcc
LD       = $(GCCPATH)/bin/$(CROSS)ld
OBJCOPY  = $(GCCPATH)/bin/$(CROSS)objcopy

CFLAGS = -Wall -O2 -ffreestanding -nostdinc -nostdlib -nostartfiles \
         -mcpu=cortex-a76 -mgeneral-regs-only

# $(SDCARD)/bootfs is the FAT32 partition on the Pi-5 SD card.
# Override e.g. `make install SDCARD=/Volumes/RPI5SD`.
SDCARD ?= /Volumes
DEST    = $(SDCARD)/bootfs

all: kernel_2712.img

boot.o: boot.S
	$(CC) $(CFLAGS) -c boot.S -o boot.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

kernel_2712.img: boot.o $(OFILES) link.ld
	$(LD) -nostdlib boot.o $(OFILES) -T link.ld -o kernel_2712.elf
	$(OBJCOPY) -O binary kernel_2712.elf kernel_2712.img
	@ls -la kernel_2712.img

# ----- QEMU `virt` variant -----------------------------------------
# Same sources, two compile-time overrides: PL011 base lives at
# 0x09000000 on the virt machine, and the binary is loaded at the
# RAM_base + 0x80000 = 0x40080000 offset that QEMU's -kernel uses.
QEMU_CFLAGS = $(CFLAGS) -DUART0_BASE=0x09000000UL
QEMU_OBJS   = $(CFILES:.c=.qemu.o) boot.qemu.o

%.qemu.o: %.c
	$(CC) $(QEMU_CFLAGS) -c $< -o $@

boot.qemu.o: boot.S
	$(CC) $(QEMU_CFLAGS) -c boot.S -o boot.qemu.o

kernel_virt.img: $(QEMU_OBJS) link_virt.ld
	$(LD) -nostdlib $(QEMU_OBJS) -T link_virt.ld -o kernel_virt.elf
	$(OBJCOPY) -O binary kernel_virt.elf kernel_virt.img
	@ls -la kernel_virt.img

# Interactive run: Ctrl-A X to quit; Ctrl-A C for QEMU monitor.
QEMU       ?= qemu-system-aarch64
QEMU_FLAGS  = -M virt -cpu cortex-a76 -m 256M -nographic -kernel kernel_virt.img

qemu: kernel_virt.img
	@echo ">>> launching QEMU virt (Ctrl-A X to quit) <<<"
	$(QEMU) $(QEMU_FLAGS)

# Non-interactive smoke: pipe a few commands in.  The leading "\n\n"
# is a sacrificial empty line that absorbs the LCRH-write FIFO reset
# at boot (QEMU starts pumping stdin before our UART init finishes,
# so the first byte after open() is consistently dropped).  The shell
# silently re-prompts on empty input, so no harm done.
qemu-smoke: kernel_virt.img
	printf '\n\nhello\nmem\npeek 0x09000018\nuptime\nhelp\necho smoke ok\n' | \
	  timeout 10 $(QEMU) $(QEMU_FLAGS) | tee qemu-smoke.log

install: kernel_2712.img
	sudo rm -f $(DEST)/kernel_*.img
	sudo cp kernel_2712.img $(DEST)/
	sudo cp ../sdcard/config.txt $(DEST)/
	sync
	sudo umount $(DEST) || true

clean:
	rm -f *.o *.elf *.img *.qemu.o qemu-smoke.log
