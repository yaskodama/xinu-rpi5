// system/chainload.c — network kexec for the Pi 5.
//
// Boots a kernel image that was POSTed over HTTP (staged in RAM at 0x4000000)
// WITHOUT a power cycle or an SD/USB write: relocate the chainload trampoline to
// a safe high address, flush caches, and jump.  The trampoline (loader/
// chainload.S) disables the MMU + caches, copies the staged image to the
// firmware load address 0x80000, and branches there; the new kernel's boot.S
// re-initialises everything.  RAM-only, so a bad image just needs a real
// power-cycle (no brick risk) — and a good one means we never reflash again.

extern unsigned char chainload_stub[];
extern unsigned char chainload_stub_end[];
extern unsigned char chainload_park[];
extern unsigned char chainload_park_end[];

void uart_puts(const char *s);
void uart_putc(char c);

/* Safe scratch, well clear of both 0x80000 (the copy destination) and the
 * heap staging buffer.  0x10000000 = 256 MB. */
#define CHAIN_SAFE      0x10000000UL
#define CHAIN_PARK      0x10001000UL    /* park trampoline, its own page   */
#define CHAIN_ACK       0x10002000UL    /* one ack byte per secondary core */

void kernel_chainload(unsigned long stage, unsigned long len)
{
    volatile unsigned char *safe = (volatile unsigned char *)CHAIN_SAFE;
    volatile unsigned char *park = (volatile unsigned char *)CHAIN_PARK;
    volatile unsigned char *ack  = (volatile unsigned char *)CHAIN_ACK;
    int stublen = (int)(chainload_stub_end - chainload_stub);
    int parklen = (int)(chainload_park_end - chainload_park);

    uart_puts("chainload: jumping to staged kernel at 0x80000 (no return)\n");

    /* Relocate both trampolines before touching anything else: the park one
     * has to be live before the secondaries are asked to jump to it. */
    for (int i = 0; i < stublen; i++) safe[i] = chainload_stub[i];
    for (int i = 0; i < parklen; i++) park[i] = chainload_park[i];
    for (int i = 0; i < 8; i++)       ack[i]  = 0;
    for (unsigned long a = CHAIN_PARK; a < CHAIN_PARK + (unsigned long)parklen; a += 64) {
        __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
        __asm__ volatile ("ic ivau,  %0" :: "r"(a) : "memory");
    }
    __asm__ volatile ("dsb sy\n isb" ::: "memory");

    /* Stop cores 1-3.  They idle in smp_worker_loop(), which lives in the
     * .text we are about to overwrite — left running they execute code being
     * rewritten under them, with page tables that go the same way.  This is
     * why chainloading used to take the board down every time. */
    {
        extern int smp_park_secondaries(unsigned long, volatile unsigned char *);
        int stopped = smp_park_secondaries(CHAIN_PARK, ack);
        uart_puts("chainload: secondaries stopped = ");
        uart_putc((char)('0' + (stopped < 0 ? 0 : (stopped > 9 ? 9 : stopped))));
        uart_puts("\n");
    }

    /* Mask all interrupts — nothing must fire once we start tearing down. */
    __asm__ volatile ("msr daifset, #0xf" ::: "memory");

    /* Cache maintenance (caches are off, but be correct anyway): push the staged
     * image + the destination to RAM, and make the relocated stub I-coherent. */
    for (unsigned long a = stage; a < stage + len; a += 64)
        __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    for (unsigned long a = 0x80000UL; a < 0x80000UL + len; a += 64)
        __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    for (unsigned long a = (unsigned long)safe; a < (unsigned long)safe + stublen; a += 64) {
        __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
        __asm__ volatile ("ic ivau,  %0" :: "r"(a) : "memory");
    }
    __asm__ volatile ("dsb sy" ::: "memory");
    __asm__ volatile ("isb" ::: "memory");

    /* Jump: stub(src=stage, dst=0x80000, len). */
    typedef void (*chain_fn)(unsigned long, unsigned long, unsigned long);
    ((chain_fn)(unsigned long)safe)(stage, 0x80000UL, len);
    /* unreachable */
    for (;;) {}
}
