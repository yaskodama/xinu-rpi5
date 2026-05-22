// kernel/uart.h — minimal PL011 UART0 interface for Raspberry Pi 5.
//
// Mapped into the BCM2712 high-memory MMIO window — Pi 5 moves all
// peripherals to 0x107C000000 (Pi 4 was 0xFE000000).  The PL011 UART0
// CR/DR/FR register block sits 0x1001000 above the base, so the absolute
// register base is 0x107D001000.
//
// The header is just three functions:
//   uart_init() — claim GPIO14/15, deassert UART, set 115200/8N1, enable
//   uart_putc(c) — block until TX FIFO has space, then write one char
//   uart_puts(s) — convenience wrapper that walks a NUL-terminated string
//                  and converts each '\n' into "\r\n" for terminal sanity.

#ifndef XINU_RPI5_UART_H
#define XINU_RPI5_UART_H

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);

#endif /* XINU_RPI5_UART_H */
