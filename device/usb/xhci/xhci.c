// device/usb/xhci/xhci.c — VL805 xHCI driver, phase XHCI-A (probe).
//
// Today this file:
//   1. Tries VC mailbox tag 0x00030058 (notify-xhci-reset).  On Pi 4
//      firmware this re-runs the VL805 bring-up sequence — Linux's
//      xhci-pci.c does the same thing when it sees vendor 0x1106
//      device 0x3483.
//   2. Probes the BCM2711 PCIe-1 controller MMIO at several offsets
//      (revision, status, link cap, root-port vendor) so we can see
//      from the shell-window log which registers respond and which
//      don't.
//
// All output goes through uart_puts() so it shows up both on the
// (absent) UART cable and in the on-screen shell window.

#include "xhci.h"
#include "uart.h"
#include "mbox.h"

#ifdef PCIE_BASE

#define PCIE_REG(off)         (*(volatile unsigned int *)(PCIE_BASE + (off)))
#define PCIE_RC_CFG_VENDOR    0x0000  /* root-complex CFG: PCI vendor/device */
#define PCIE_RC_CFG_CLASS     0x0008
#define PCIE_MISC_CTRL        0x4008
#define PCIE_MISC_REVISION    0x406C
#define PCIE_MISC_STATUS      0x4068
#define PCIE_MISC_HARD_DBG    0x4204
#define PCIE_RC_CFG_LINK_CAP  0x04DC

static void puts_hex32(unsigned int v)
{
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        unsigned int nyb = (v >> ((7 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10));
    }
    buf[10] = 0;
    uart_puts(buf);
}

static int xhci_notify_reset(void)
{
    /* Property tag 0x00030058 (notify-xhci-reset).  Previous attempt with
     * devid=0x00100000 hung the firmware (mailbox call never returned -> all
     * subsequent mailbox calls also dead).  Try devid=0 (firmware default,
     * which the original comment said was the fallback): on Pi 4 firmware,
     * VL805 is the only xHCI client of this tag, so default-targets-VL805
     * should work — if it does we'll know the encoding was the issue, and
     * if it hangs again the tag itself is the wrong path on this firmware
     * and we need self-bring-up (CPRMAN + brcmstb-pcie). */
    static volatile unsigned int __attribute__((aligned(16))) buf[8];
    buf[0] = 32;
    buf[1] = 0;
    buf[2] = 0x00030058U;        /* notify-xhci-reset */
    buf[3] = 4;
    buf[4] = 0;
    buf[5] = 0;                  /* devid: 0 = firmware default (VL805) */
    buf[6] = 0;
    buf[7] = 0;
    return mbox_call(buf);
}

unsigned int xhci_pcie_revision(void)
{
    return PCIE_REG(PCIE_MISC_REVISION);
}

static void dump_pcie_probe(const char *label)
{
    uart_puts("xhci: --- "); uart_puts(label); uart_puts(" ---\n");
    uart_puts("  vendor[0x000] = ");  puts_hex32(PCIE_REG(PCIE_RC_CFG_VENDOR));   uart_puts("\n");
    uart_puts("  class [0x008] = ");  puts_hex32(PCIE_REG(PCIE_RC_CFG_CLASS));    uart_puts("\n");
    uart_puts("  ctrl  [0x4008]= ");  puts_hex32(PCIE_REG(PCIE_MISC_CTRL));       uart_puts("\n");
    uart_puts("  stat  [0x4068]= ");  puts_hex32(PCIE_REG(PCIE_MISC_STATUS));     uart_puts("\n");
    uart_puts("  rev   [0x406C]= ");  puts_hex32(PCIE_REG(PCIE_MISC_REVISION));   uart_puts("\n");
    uart_puts("  dbg   [0x4204]= ");  puts_hex32(PCIE_REG(PCIE_MISC_HARD_DBG));   uart_puts("\n");
    uart_puts("  lcap  [0x04DC]= ");  puts_hex32(PCIE_REG(PCIE_RC_CFG_LINK_CAP)); uart_puts("\n");
}

void xhci_init(void)
{
    uart_puts("xhci: BCM2711 PCIe-1 base = ");
    puts_hex32((unsigned int)PCIE_BASE);
    uart_puts("\n");

    dump_pcie_probe("pre-mailbox");

    int rc = xhci_notify_reset();
    uart_puts("xhci: notify-xhci-reset rc = ");
    puts_hex32((unsigned int)rc);
    uart_puts("\n");

    dump_pcie_probe("post-mailbox");
}

/* On-demand /xhci-reset HTTP route: do the VC mailbox call separately so we
 * can see whether it returns or hangs (the boot-time variant wedged the box). */
int xhci_notify_reset_call(void) { return xhci_notify_reset(); }

/* Bare PCIe-controller register dump into a text buffer; called by the /pcie
 * HTTP route.  Reads that fault are caught by the sync-exception handler
 * (recover_spin) so the box stays alive across iterations. */
static int s_put(char *b, int p, int max, const char *s)
{
    while (*s && p < max - 1) b[p++] = *s++;
    return p;
}
static int s_puthex32(char *b, int p, int max, unsigned int v)
{
    if (p < max - 1) b[p++] = '0';
    if (p < max - 1) b[p++] = 'x';
    for (int i = 7; i >= 0 && p < max - 1; i--) {
        unsigned int n = (v >> (i * 4)) & 0xF;
        b[p++] = (char)(n < 10 ? '0' + n : 'a' + (n - 10));
    }
    return p;
}
/* Fault-resilient MMIO read (system/exception.c).  Returns 0 / -1. */
extern int safe_mmio_read32(unsigned long addr, unsigned int *out);

int xhci_pcie_dump_html(char *out, int max)
{
    int p = 0;
    static const struct { const char *name; unsigned int off; } regs[] = {
        { "vendor[0x000] ", PCIE_RC_CFG_VENDOR   },
        { "class [0x008] ", PCIE_RC_CFG_CLASS    },
        { "ctrl  [0x4008]", PCIE_MISC_CTRL       },
        { "stat  [0x4068]", PCIE_MISC_STATUS     },
        { "rev   [0x406C]", PCIE_MISC_REVISION   },
        { "dbg   [0x4204]", PCIE_MISC_HARD_DBG   },
        { "lcap  [0x04DC]", PCIE_RC_CFG_LINK_CAP },
    };
    p = s_put(out, p, max, "pcie_base=");
    p = s_puthex32(out, p, max, (unsigned int)PCIE_BASE);
    p = s_put(out, p, max, "\n");
    for (unsigned i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
        p = s_put(out, p, max, regs[i].name);
        p = s_put(out, p, max, " = ");
        unsigned int v;
        if (safe_mmio_read32(PCIE_BASE + regs[i].off, &v) == 0) {
            p = s_puthex32(out, p, max, v);
        } else {
            p = s_put(out, p, max, "FAULT");
        }
        p = s_put(out, p, max, "\n");
    }
    if (p < max) out[p] = 0;
    return p;
}

#endif /* PCIE_BASE */

#ifndef PCIE_BASE
int xhci_notify_reset_call(void)             { return -1; }
int xhci_pcie_dump_html(char *out, int max)
{
    const char *s = "pcie: not supported on this build\n";
    int p = 0; while (*s && p < max - 1) out[p++] = *s++;
    if (p < max) out[p] = 0;
    return p;
}
#endif
