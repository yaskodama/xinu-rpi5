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
#include "shell.h"
#include "memory.h"
#include "proc.h"

extern unsigned char _end[];   /* set by link.ld — top of static image */

#ifndef HEAP_END
/* Pi 5 firmware: assume at least 1 GiB of RAM mapped starting at 0
 * (config.txt's `arm_64bit=1` gives us the whole low region).  QEMU
 * `virt` builds override this from the Makefile to 0x50000000 (256 MB). */
#define HEAP_END 0x40000000UL
#endif

static void puts_kb(unsigned long bytes)
{
    /* Tiny KB pretty-printer to avoid pulling printf in this early. */
    unsigned long kb = bytes >> 10;
    char buf[12];
    int n = 0;
    if (kb == 0) { uart_putc('0'); return; }
    while (kb > 0) { buf[n++] = (char)('0' + (kb % 10)); kb /= 10; }
    while (n--) uart_putc(buf[n]);
}

void kernel_main(void)
{
    unsigned long heap_start;

    uart_init();

    uart_puts("\n");
    uart_puts("================================================\n");
    uart_puts("  Xinu Pi5 hello (AArch64, BCM2712, kernel_2712.img)\n");
    uart_puts("  PL011 UART0 @ 0x107D001000, 115200 8N1\n");
    uart_puts("  bootstrap: leex-style stub + xinu-rpi5 main\n");
    uart_puts("================================================\n");
    uart_puts("\n");

    /* M1 — bring up the first-fit kernel heap between _end and
     *      HEAP_END.  Everything after this point (proc stacks,
     *      shell command-time allocations) goes through getmem(). */
    heap_start = (unsigned long)_end;
    mem_init(heap_start, HEAP_END);
    uart_puts("heap: ");
    puts_kb(mem_total_bytes());
    uart_puts(" KiB available\n");

    /* S0 — initialise the process table.  Slot 0 (NULLPROC) inherits
     *      the current execution context: it is the boot/shell thread,
     *      runs whenever the ready list is empty, never sits on it. */
    proc_init();
    uart_puts("sched: cooperative ctxsw ready (NULLPROC = shell)\n");

    uart_puts("\n");
    uart_puts("Round 1: B/U/M1/S0 done — entering interactive shell.\n");
    uart_puts("Next milestones: M0 MMU, S1 GIC+timer\n");
    uart_puts("\n");

    /* Hand off to the bare-metal REPL (never returns). */
    shell_main();
}
