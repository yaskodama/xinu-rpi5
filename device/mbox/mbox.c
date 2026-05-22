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

int mbox_call(volatile unsigned int *buf)
{
#ifdef SKIP_MBOX
    /* QEMU virt has no VC mailbox at our MMIO address.  Without an
     * exception vector table the first stray load/store would trap
     * to VBAR_EL1=0 and brick the kernel; bail out cleanly instead. */
    (void)buf;
    return -1;
#else
    unsigned long t;
    unsigned int msg;

    /* Build the message: low 4 bits = channel, rest = buf phys addr.
     * Buffer must be 16-byte aligned so the low bits are free. */
    msg = ((unsigned int)(unsigned long)buf & ~0xFu) | MBOX_CHANNEL_PROP;

    /* Wait for write slot. */
    for (t = 0; t < MBOX_TIMEOUT; t++) {
        if (!(MBOX_STATUS & MBOX_STATUS_FULL)) break;
    }
    if (t == MBOX_TIMEOUT) return -1;

    MBOX_WRITE = msg;

    /* Wait for response on the same channel.  Loop budget caps the
     * "wrong channel — drop and retry" path so we can't spin forever
     * on platforms where the MMIO is unmapped (e.g. QEMU virt always
     * reads zero, making EMPTY=false but never matching channel 8). */
    int attempts;
    for (attempts = 0; attempts < 16; attempts++) {
        for (t = 0; t < MBOX_TIMEOUT; t++) {
            if (!(MBOX_STATUS & MBOX_STATUS_EMPTY)) break;
        }
        if (t == MBOX_TIMEOUT) return -1;

        unsigned int reply = MBOX_READ;
        if ((reply & 0xF) == MBOX_CHANNEL_PROP) {
            /* Tag protocol: buf[1] contains response code; bit 31
             * set = success.  Caller cares about the rest. */
            return (buf[1] == 0x80000000) ? 0 : -1;
        }
        /* Wrong channel — drop and try again. */
    }
    return -1;
#endif
}
