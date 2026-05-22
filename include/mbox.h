// include/mbox.h — VideoCore mailbox driver interface.
//
// Standard "property tags" channel (channel 8) for talking to the
// firmware about HDMI / framebuffer / clocks / etc.  Works on every
// Pi generation that exposes the legacy VC mailbox; we use it here
// to ask Pi 5's firmware for a framebuffer it has already set up.
//
// The MMIO base address is best-effort for Pi 5 (BCM2712):
// peripheral base 0x107C000000 + the conventional 0xB880 offset.
// If a future Pi or a Pi 5 revision moves it, override MBOX_BASE
// via the Makefile (-DMBOX_BASE=0x...UL).

#ifndef XINU_RPI5_MBOX_H
#define XINU_RPI5_MBOX_H

#ifndef MBOX_BASE
#define MBOX_BASE  0x107C00B880UL
#endif

/* Send a property-tag buffer.  `buf` must be 16-byte aligned and
 * formatted per the VC mailbox property interface.  Returns 0 on
 * success, -1 on timeout (no response from firmware).  Channel 8
 * is implicit. */
int mbox_call(volatile unsigned int *buf);

#endif /* XINU_RPI5_MBOX_H */
