# compile/Makefile — top-level build for xinu-rpi5.
#
# Mirrors the layout convention used by github.com/davidxyz/xinuPi
# (and Embedded Xinu in general): sources live under per-subsystem
# directories one level up (loader/, mem/, system/, shell/, device/),
# headers live in ../include/, and *every* build artefact ends up in
# this directory (objects in obj/, ELFs + flat images at the root).
#
# Targets:
#   make            real Pi 5 image  (compile/kernel_2712.img)
#   make qemu       interactive QEMU virt (Ctrl-A X to quit)
#   make qemu-smoke canned-input smoke run; writes qemu-smoke.log
#   make install SDCARD=/Volumes
#   make clean
#
# Run from this directory:
#     cd compile && make
#
# Source tree (relative to ..):
#   loader/        boot.S + main.c   (AArch64 entry + kernel_main)
#   mem/           memory.c          (first-fit getmem/freemem)
#   system/        proc.c + ctxsw.S  (proctab + scheduler + ctxsw)
#   shell/         shell.c           (bare-metal REPL)
#   device/uart/   uart.c            (PL011 UART0 driver)
#   include/       *.h               (shared headers)

# ----- toolchain ------------------------------------------------------
# Homebrew installs either `aarch64-elf-gcc` (gcc-aarch64-elf formula)
# or `aarch64-none-elf-gcc` (arm-toolchain cask); auto-detect.
GCCPATH ?= /opt/homebrew
CROSS   ?= $(shell test -x $(GCCPATH)/bin/aarch64-elf-gcc && \
                   echo aarch64-elf- || echo aarch64-none-elf-)
CC       = $(GCCPATH)/bin/$(CROSS)gcc
LD       = $(GCCPATH)/bin/$(CROSS)ld
OBJCOPY  = $(GCCPATH)/bin/$(CROSS)objcopy

# ----- source discovery -----------------------------------------------
COMPONENTS = loader mem system shell device/uart device/mbox device/video

C_SRCS := $(foreach d,$(COMPONENTS),$(wildcard ../$(d)/*.c))
S_SRCS := $(foreach d,$(COMPONENTS),$(wildcard ../$(d)/*.S))

# Flatten the object files into obj/ so the pattern rules stay simple.
# Name collisions (two .c files with the same basename) would manifest
# here as a duplicate object — keep basenames unique across COMPONENTS.
C_OBJS := $(patsubst %.c,obj/%.o,$(notdir $(C_SRCS)))
S_OBJS := $(patsubst %.S,obj/%.o,$(notdir $(S_SRCS)))
OBJS   := $(C_OBJS) $(S_OBJS)

# QEMU variant uses obj/qemu/ so the real-hardware build isn't clobbered.
Q_C_OBJS := $(patsubst %.c,obj/qemu/%.o,$(notdir $(C_SRCS)))
Q_S_OBJS := $(patsubst %.S,obj/qemu/%.o,$(notdir $(S_SRCS)))
Q_OBJS   := $(Q_C_OBJS) $(Q_S_OBJS)

# VPATH lets `obj/%.o : %.c` find the .c file regardless of subdir.
VPATH := $(addprefix ../,$(COMPONENTS))

# ----- flags ----------------------------------------------------------
INCLUDE = -I../include

# -mgeneral-regs-only keeps ctxsw.S free from having to spill v0-v31.
# HEAP_END defaults to 1 GiB (Pi-5 conservative); QEMU build bumps it.
CFLAGS  = -Wall -O2 -ffreestanding -nostdinc -nostdlib -nostartfiles \
          -mcpu=cortex-a76 -mgeneral-regs-only \
          $(INCLUDE) \
          -DHEAP_END=0x40000000UL

QEMU_CFLAGS = $(CFLAGS) -DUART0_BASE=0x09000000UL \
              -UHEAP_END -DHEAP_END=0x50000000UL \
              -DSKIP_MBOX

# ----- pattern rules --------------------------------------------------
obj:
	@mkdir -p obj

obj/qemu:
	@mkdir -p obj/qemu

obj/%.o: %.c | obj
	$(CC) $(CFLAGS) -c $< -o $@

obj/%.o: %.S | obj
	$(CC) $(CFLAGS) -c $< -o $@

obj/qemu/%.o: %.c | obj/qemu
	$(CC) $(QEMU_CFLAGS) -c $< -o $@

obj/qemu/%.o: %.S | obj/qemu
	$(CC) $(QEMU_CFLAGS) -c $< -o $@

# ----- real Pi 5 image ------------------------------------------------
all: kernel_2712.img

kernel_2712.img: $(OBJS) link.ld
	$(LD) -nostdlib $(OBJS) -T link.ld -o kernel_2712.elf
	$(OBJCOPY) -O binary kernel_2712.elf kernel_2712.img
	@ls -la kernel_2712.img

# ----- QEMU virt image ------------------------------------------------
kernel_virt.img: $(Q_OBJS) link_virt.ld
	$(LD) -nostdlib $(Q_OBJS) -T link_virt.ld -o kernel_virt.elf
	$(OBJCOPY) -O binary kernel_virt.elf kernel_virt.img
	@ls -la kernel_virt.img

# ----- QEMU run targets ----------------------------------------------
# Interactive: -nographic ties stdin to the UART; Ctrl-A X to quit,
# Ctrl-A C for the QEMU monitor (`q` to return).
# Smoke    : -display none -serial stdio behaves more predictably
#            with piped stdin than -nographic.
QEMU            ?= qemu-system-aarch64
QEMU_COMMON      = -M virt -cpu cortex-a76 -m 256M -kernel kernel_virt.img
QEMU_FLAGS       = $(QEMU_COMMON) -nographic
QEMU_SMOKE_FLAGS = $(QEMU_COMMON) -display none -serial stdio

qemu: kernel_virt.img
	@echo ">>> launching QEMU virt (Ctrl-A X to quit) <<<"
	$(QEMU) $(QEMU_FLAGS)

# The leading "\n\n" is a sacrificial empty line that absorbs the
# LCRH-write FIFO reset at boot (QEMU pumps stdin before our UART init
# finishes; the first byte after open() is consistently dropped).
qemu-smoke: kernel_virt.img
	printf '\n\nhello\nmem\npeek 0x09000018\nuptime\nps\npingpong 3\nprocdemo 3\nps\necho --- smoke ok ---\nhalt\n' | \
	  timeout 15 $(QEMU) $(QEMU_SMOKE_FLAGS) | tee qemu-smoke.log

# ----- install onto a Pi 5 SD card -----------------------------------
SDCARD ?= /Volumes
DEST    = $(SDCARD)/bootfs

install: kernel_2712.img
	sudo rm -f $(DEST)/kernel_*.img
	sudo cp kernel_2712.img $(DEST)/
	sudo cp ../sdcard/config.txt $(DEST)/
	sync
	sudo umount $(DEST) || true

# ----- housekeeping --------------------------------------------------
clean:
	rm -rf obj kernel_*.elf kernel_*.img qemu-smoke.log

.PHONY: all qemu qemu-smoke install clean
