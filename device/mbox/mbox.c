// device/mbox/mbox.c — minimal VideoCore mailbox driver.
//
// MMIO register block (relative to MBOX_BASE):
//   0x00  read    — mailbox 0 read (firmware -> us)
//   0x18  status  — bit 31 (FULL) blocks write; bit 30 (EMPTY) blocks read
//   0x20  write   — mailbox 0 write (us -> firmware)
//
// Calling protocol for channel 8 ("property tags"):
//   1. Build a request buffer (16-byte aligned, ends with end-tag 0).
//   2. Loop until !FULL, write (buf_phys | channel).
//   3. Loop until !EMPTY, read; verify low 4 bits == channel.
//
// Spin loops are bounded so we degrade gracefully on platforms
// where the mailbox doesn't exist (e.g. QEMU virt): timing out
// returns -1, and the caller can fall back to UART-only output.

#include "mbox.h"

#define MBOX_READ        (*(volatile unsigned int *)(MBOX_BASE + 0x00))
#define MBOX_STATUS      (*(volatile unsigned int *)(MBOX_BASE + 0x18))
#define MBOX_WRITE       (*(volatile unsigned int *)(MBOX_BASE + 0x20))

#define MBOX_STATUS_FULL   (1u << 31)
#define MBOX_STATUS_EMPTY  (1u << 30)

#define MBOX_CHANNEL_PROP  8

/* Timeout for either direction.  At ~1 ns/iter on A76 this is a
 * few hundred ms — long enough for a real firmware response, short
 * enough that a missing mailbox doesn't hang the boot. */
#define MBOX_TIMEOUT       100000000UL

#ifndef SKIP_MBOX
/* Send `buf` on the property channel, forming the GPU-visible address as
 * (phys & ~0xF) | bus_or.  Returns 0 on a success response (buf[1] bit31). */
static int mbox_xfer(volatile unsigned int *buf, unsigned int bus_or)
{
    unsigned long t;
    unsigned int msg =
        (((unsigned int)(unsigned long)buf & ~0xFu) | bus_or) | MBOX_CHANNEL_PROP;

    /* Wait for write slot. */
    for (t = 0; t < MBOX_TIMEOUT; t++) {
        if (!(MBOX_STATUS & MBOX_STATUS_FULL)) break;
    }
    if (t == MBOX_TIMEOUT) return -1;

    MBOX_WRITE = msg;

    /* Wait for a response on our channel.  Loop budget caps the
     * "wrong channel — drop and retry" path so we can't spin forever
     * on platforms where the MMIO is unmapped (QEMU virt reads zero,
     * making EMPTY=false but never matching channel 8). */
    int attempts;
    for (attempts = 0; attempts < 16; attempts++) {
        for (t = 0; t < MBOX_TIMEOUT; t++) {
            if (!(MBOX_STATUS & MBOX_STATUS_EMPTY)) break;
        }
        if (t == MBOX_TIMEOUT) return -1;

        unsigned int reply = MBOX_READ;
        if ((reply & 0xF) == MBOX_CHANNEL_PROP) {
            /* Tag protocol: buf[1] = response code; bit 31 = success. */
            return (buf[1] == 0x80000000) ? 0 : -1;
        }
        /* Wrong channel — drop and try again. */
    }
    return -1;
}

#ifdef MBOX_PROBE_BUS
/* Pi 5 (BCM2712): the address we write to the mailbox must be in the
 * VideoCore's view of memory, and unlike the Pi 1-4 the working alias is
 * not known a priori.  Probe once with a harmless GET_FIRMWARE_REVISION,
 * trying the legacy uncached alias, the cached alias, then raw physical;
 * cache the first that the firmware acknowledges and reuse it for every
 * later call (FB allocation, etc.).  Mirrors the proven first-light path. */
static unsigned int mbox_bus_or = 0xFFFFFFFFu;   /* 0xFFFFFFFF = not yet probed */

static void mbox_probe_bus(void)
{
    static volatile unsigned int pb[8] __attribute__((aligned(16)));
    static const unsigned int conv[3] = { 0xC0000000u, 0x40000000u, 0x00000000u };
    for (int k = 0; k < 3; k++) {
        pb[0] = 7 * 4;        /* total size (7 words)        */
        pb[1] = 0;            /* request                     */
        pb[2] = 0x00000001;   /* tag: GET_FIRMWARE_REVISION  */
        pb[3] = 4;            /* value buffer size           */
        pb[4] = 0;            /* request code                */
        pb[5] = 0;            /* value (firmware fills in)   */
        pb[6] = 0;            /* end tag                     */
        if (mbox_xfer(pb, conv[k]) == 0) { mbox_bus_or = conv[k]; return; }
    }
    mbox_bus_or = 0x00000000u;   /* nothing acked — fall back to raw phys */
}
#endif
#endif  /* !SKIP_MBOX */

int mbox_call(volatile unsigned int *buf)
{
#ifdef SKIP_MBOX
    /* QEMU virt has no VC mailbox at our MMIO address.  Without an
     * exception vector table the first stray load/store would trap
     * to VBAR_EL1=0 and brick the kernel; bail out cleanly instead. */
    (void)buf;
    return -1;
#elif defined(MBOX_PROBE_BUS)
    if (mbox_bus_or == 0xFFFFFFFFu) mbox_probe_bus();
    return mbox_xfer(buf, mbox_bus_or);
#else
    /* Pi 4 / Pi 3: raw physical address works (1:1 VideoCore view). */
    return mbox_xfer(buf, 0u);
#endif
}
