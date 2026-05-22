// kernel/uart.c — bare-metal PL011 UART0 driver for Raspberry Pi 5.
//
// This is the first I/O Xinu has available on Pi 5; everything else
// (the framebuffer mailbox, the eMMC, network via RP1) comes later.
// We rely on the firmware having already enabled UART0 via config.txt
// (enable_uart=1 + dtparam=uart0=on) which locks the UART clock to a
// stable 48 MHz reference, so we only need to:
//
//   1. Disable the UART so we can reprogram safely.
//   2. Configure GPIO14 (TXD) and GPIO15 (RXD) as ALT0 (PL011).
//      On Pi 5 the GPIO controller lives inside RP1 — but the
//      firmware's enable_uart=1 has already done the muxing for us,
//      so we can skip GPIO programming on the first cut and add it
//      in a later phase if the cable misbehaves.
//   3. Program 115200/8N1: integer baud divisor 26, fractional 3
//      ((48e6) / (16 * 115200) = 26.0416…).
//   4. Re-enable the UART.
//
// Once initialised, putc spins on the TX-FIFO-full flag and writes
// one byte to DR.  No interrupts here — this is the absolute earliest
// I/O channel and the kernel proper hasn't even set up an exception
// table yet.

#include "uart.h"

/* BCM2712 high-memory MMIO base + PL011 UART0 offset.
 * (Pi 4's base was 0xFE000000 + 0x201000 — both moved on Pi 5.)
 *
 * Overridable at compile time via -DUART0_BASE=0xNN so the same
 * source builds for QEMU `virt` (PL011 at 0x09000000) without
 * touching the real-hardware default.  Don't use this for actual
 * Pi 4/Pi 3 — those need GPIO ALT muxing that the firmware does
 * for us on Pi 5 + QEMU virt. */
#ifndef UART0_BASE
#define UART0_BASE   0x107D001000UL
#endif

#define UART_DR      (*(volatile unsigned int *)(UART0_BASE + 0x00))
#define UART_FR      (*(volatile unsigned int *)(UART0_BASE + 0x18))
#define UART_IBRD    (*(volatile unsigned int *)(UART0_BASE + 0x24))
#define UART_FBRD    (*(volatile unsigned int *)(UART0_BASE + 0x28))
#define UART_LCRH    (*(volatile unsigned int *)(UART0_BASE + 0x2C))
#define UART_CR      (*(volatile unsigned int *)(UART0_BASE + 0x30))
#define UART_ICR     (*(volatile unsigned int *)(UART0_BASE + 0x44))

#define FR_RXFE      (1u << 4)   /* RX FIFO empty  */
#define FR_TXFF      (1u << 5)   /* TX FIFO full   */
#define LCRH_FEN     (1u << 4)   /* enable FIFOs   */
#define LCRH_WLEN_8  (3u << 5)   /* 8-bit words    */
#define CR_UARTEN    (1u << 0)
#define CR_TXE       (1u << 8)
#define CR_RXE       (1u << 9)

void uart_init(void)
{
    /* 1. Disable while we reprogram. */
    UART_CR = 0;

    /* Clear any pending interrupts left from the bootrom. */
    UART_ICR = 0x7FF;

    /* 3. 115200 baud from a 48 MHz UART clock.
     *    divisor = 48e6 / (16 * 115200) = 26.0416…
     *    fractional part * 64 + 0.5 = 3
     */
    UART_IBRD = 26;
    UART_FBRD = 3;

    /* 8N1 with FIFOs enabled. */
    UART_LCRH = LCRH_FEN | LCRH_WLEN_8;

    /* 4. Re-enable TX + RX. */
    UART_CR = CR_UARTEN | CR_TXE | CR_RXE;

    /* Drain any bytes the firmware / QEMU stdio pumped in before we
     * finished reprogramming — otherwise piped-stdin smoke tests
     * (and the rare USB-serial chatter) lose their first byte to
     * the LCRH-write FIFO reset.  Harmless on a real cable. */
    while (!(UART_FR & FR_RXFE)) {
        (void)UART_DR;
    }
}

void uart_putc(char c)
{
    /* Busy-wait until the TX FIFO has room. */
    while (UART_FR & FR_TXFF) {
        /* spin */
    }
    UART_DR = (unsigned int)(unsigned char)c;
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}

char uart_getc(void)
{
    /* Busy-wait until the RX FIFO has something. */
    while (UART_FR & FR_RXFE) {
        /* spin */
    }
    return (char)(UART_DR & 0xFF);
}

/* Line editor: blocking, echo + backspace + DEL.  CR/LF terminates. */
int uart_getline(char *buf, int max)
{
    int n = 0;
    if (max <= 0) {
        return 0;
    }
    while (n < max - 1) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') {
            uart_puts("\n");
            buf[n] = 0;
            return n;
        }
        if (c == 0x08 || c == 0x7F) {  /* BS / DEL */
            if (n > 0) {
                n--;
                /* Visually erase: backspace, space, backspace. */
                uart_putc('\b');
                uart_putc(' ');
                uart_putc('\b');
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {   /* printable */
            buf[n++] = c;
            uart_putc(c);              /* echo */
        }
        /* Silently swallow other control chars. */
    }
    buf[n] = 0;
    return n;
}
