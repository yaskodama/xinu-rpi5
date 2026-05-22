// kernel/main.c — Xinu-on-Pi-5 first sign of life.
//
// boot.S has cleared BSS, set up the initial stack and dropped here.
// All we do for the B0/B1/B2/U0/U1 milestone is bring up UART0 and
// print a hello banner so the USB-serial cable shows something the
// host can grep for ("Xinu Pi5 hello" is the smoke marker).
//
// Real Xinu init (interrupts, mmu, scheduler) lands in subsequent
// phases — those will pull in their own files (system/initialize.c,
// arch/aarch64/mmu.c, arch/aarch64/exception_vectors.S, …).  For now
// the kernel just hangs in a WFE loop after the banner so the human
// has time to read it before resetting the board.

#include "uart.h"

void kernel_main(void)
{
    uart_init();

    uart_puts("\n");
    uart_puts("================================================\n");
    uart_puts("  Xinu Pi5 hello (AArch64, BCM2712, kernel_2712.img)\n");
    uart_puts("  PL011 UART0 @ 0x107D001000, 115200 8N1\n");
    uart_puts("  bootstrap: leex-style stub + xinu-rpi5 main\n");
    uart_puts("================================================\n");
    uart_puts("\n");
    uart_puts("kernel_main: parked in WFE loop (Round 1 phase B/U done)\n");
    uart_puts("Next milestones: M0 MMU, S0 ctxsw, S1 GIC+timer\n");

    /* Park.  boot.S would do this too if we returned, but explicit
     * is friendlier when scrolling the disassembly. */
    for (;;) {
        __asm__ volatile ("wfe");
    }
}
