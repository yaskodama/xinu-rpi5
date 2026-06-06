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

void uart_puts(const char *s);

void kernel_chainload(unsigned long stage, unsigned long len)
{
    volatile unsigned char *safe = (volatile unsigned char *)0x10000000UL; /* 256 MB, clear of 0x80000 dst */
    int stublen = (int)(chainload_stub_end - chainload_stub);

    uart_puts("chainload: jumping to staged kernel at 0x80000 (no return)\n");

    /* Mask all interrupts — nothing must fire once we start tearing down. */
    __asm__ volatile ("msr daifset, #0xf" ::: "memory");

    /* Relocate the trampoline stub to the safe address. */
    for (int i = 0; i < stublen; i++) safe[i] = chainload_stub[i];

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
