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

# Toolchain — Homebrew installs aarch64-none-elf binaries on PATH;
# leex's HOME/aarch64 layout still works if exported via GCCPATH.
GCCPATH ?= /opt/homebrew
CC       = $(GCCPATH)/bin/aarch64-none-elf-gcc
LD       = $(GCCPATH)/bin/aarch64-none-elf-ld
OBJCOPY  = $(GCCPATH)/bin/aarch64-none-elf-objcopy

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

install: kernel_2712.img
	sudo rm -f $(DEST)/kernel_*.img
	sudo cp kernel_2712.img $(DEST)/
	sudo cp ../sdcard/config.txt $(DEST)/
	sync
	sudo umount $(DEST) || true

clean:
	rm -f *.o *.elf *.img
