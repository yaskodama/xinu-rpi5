// system/pm_reset.c — BCM2711 watchdog reset.
//
// Mechanism (per BCM2835/2711 ARM peripherals, PM block at 0xFE100000):
//   PM_RSTC at +0x1c   reset control     password 0x5a000000 | full_rst
//   PM_RSTS at +0x20   reset status      password 0x5a000000 | partition
//   PM_WDOG at +0x24   watchdog timeout  password 0x5a000000 | ticks
//
// Sequence:
//   1. write PM_WDOG with a small countdown (10 ticks ≈ 64 µs)
//   2. write PM_RSTC with FULL_RST → SoC resets to partition 0
//      (firmware re-loads kernel from SD; no software state survives)
//
// References:
//   - BCM2835 ARM Peripherals manual §13 Power Management
//   - Raspberry Pi linux: drivers/watchdog/bcm2835_wdt.c

#include "pm_reset.h"

/* Pi 4 / BCM2711 PM block (also works on Pi 3 / BCM2837).  PM_BASE =
 * peripheral_base (0xFE000000 on Pi 4) + 0x100000.  Pi 5 / BCM2712 uses
 * a different SoC reset path via RP1; that variant is not implemented
 * here and will just spin in WFE. */
#define PM_BASE   0xFE100000UL
#define PM_RSTC   (PM_BASE + 0x1c)
#define PM_WDOG   (PM_BASE + 0x24)
#define PM_PASSWORD               0x5a000000U
#define PM_RSTC_WRCFG_FULL_RESET  0x20U

void pm_reset(void)
{
    volatile unsigned int *wdog = (volatile unsigned int *)PM_WDOG;
    volatile unsigned int *rstc = (volatile unsigned int *)PM_RSTC;
    /* 10 watchdog ticks ≈ tens of microseconds — short enough for an
     * obviously-immediate reset, long enough that the write completes. */
    *wdog = PM_PASSWORD | 10;
    /* Trigger FULL_RESET — partition 0 is "boot normally". */
    *rstc = PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET;
    /* SoC should reset before we get here.  Spin defensively. */
    for (;;) __asm__ volatile ("wfe");
}
