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
/* Tee uart output into `buf` (NUL-terminated, capped) until uart_capture_stop(). */
void uart_capture(char *buf, int cap);
void uart_capture_stop(void);

/* Blocking read of one byte from RX FIFO. */
char uart_getc(void);

/* Non-blocking peek of the RX FIFO.  Returns -1 if no byte is
 * available, 0..255 otherwise.  Used by the wm shell window to
 * drive the REPL from the frame loop without blocking. */
int  uart_poll_char(void);

/* Read a line into `buf` up to `max - 1` chars.  Echoes characters
 * back to the terminal so the user can see what they typed; handles
 * backspace (0x08) and DEL (0x7F).  Returns the number of bytes
 * placed in `buf` (excluding the trailing NUL).  CR (0x0D) and LF
 * (0x0A) both terminate input. */
int uart_getline(char *buf, int max);

#endif /* XINU_RPI5_UART_H */
