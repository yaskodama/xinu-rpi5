# xinu-rpi5 — top-level wrapper Makefile.
#
# The real build rules live in compile/Makefile (all objects and the flat
# kernel images are emitted there).  This wrapper only exists so the usual
# targets can be run from the repository ROOT — otherwise `make pi5` at the
# root fails with "No rule to make target ..." because the only Makefile is
# one level down in compile/.
#
# Usage (from the repo root):
#     make            # build all variants (pi4 + pi5 + qemu)
#     make pi5        # build kernel_2712.img   (Raspberry Pi 5 / BCM2712)
#     make pi5-osmin  # build kernel_min.img    (minimal OS variant)
#     make pi5-os2    # build kernel_os2.img    (kexec OS slot 2)
#     make pi5-os3    # build kernel_os3.img    (kexec OS slot 3)
#     make pi5-min    # build kernel_xinu.img   (first-light bring-up)
#     make pi4        # build kernel8.img       (Raspberry Pi 4 / BCM2711)
#     make qemu       # build kernel_virt.img   (QEMU -M virt)
#     make qemu-run   # build + launch QEMU (Ctrl-A X to quit)
#     make install_pi5 SDCARD=/Volumes
#     make clean
#
# Every goal is forwarded into compile/ via `$(MAKE) -C compile`.

COMPILE_DIR := compile

# Bare `make` builds everything, matching compile/Makefile's default.
.DEFAULT_GOAL := all

.PHONY: all pi4 pi5 pi5-osmin pi5-os2 pi5-os3 pi5-min qemu qemu-run qemu-smoke \
        install install_pi4 install_pi5 clean help

all pi4 pi5 pi5-osmin pi5-os2 pi5-os3 pi5-min qemu qemu-run qemu-smoke \
install install_pi4 install_pi5 clean help:
	$(MAKE) -C $(COMPILE_DIR) $@

# Forward any other goal (e.g. a specific image name like kernel_2712.img)
# down to compile/ as well.
%:
	$(MAKE) -C $(COMPILE_DIR) $@
