// loader/early_diag.c — paint colour blocks at candidate FB addrs.
//
// See include/early_diag.h for the why.  After Pi OS Lite was layered
// on the SD card and our kernel started actually running (rainbow gone
// → screen black instead), the original 6-candidate list wasn't dense
// enough to catch wherever the firmware allocates the HDMI FB on a
// 8 GiB Pi 5.  This version writes a 256 KiB CONTIGUOUS block of solid
// colour (no row-pitch assumption!) at 16 different candidate addresses
// covering the low 4 GiB at roughly 0.25 GiB spacing.  Pitch-independent
// means even if the FB is wider/narrower than we'd guess, the start of
// it gets stamped.

#include "early_diag.h"

#define BLOCK_BYTES    (256 * 1024)   /* 256 KiB contiguous fill      */
#define CACHE_LINE       64           /* Cortex-A76 D-cache line size */

static void clean_dcache_range(unsigned long start, unsigned long size)
{
    unsigned long end = start + size;
    /* Align start down to cache-line boundary. */
    start &= ~(CACHE_LINE - 1UL);
    for (unsigned long a = start; a < end; a += CACHE_LINE) {
        __asm__ volatile ("dc cvac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile ("dsb sy" ::: "memory");
}

static void paint_contiguous(unsigned long addr, unsigned int colour)
{
    volatile unsigned int *p = (volatile unsigned int *)addr;
    int n = BLOCK_BYTES / 4;             /* 32 bpp -> 4 B/pixel       */
    for (int i = 0; i < n; i++) p[i] = colour;
    clean_dcache_range(addr, BLOCK_BYTES);
}

void early_paint_diagnostic(void)
{
#ifdef SKIP_MBOX
    /* QEMU virt has only 256 MiB RAM (0x40000000 + 0x10000000), so
     * the higher candidates would data-abort with no exception
     * vector installed.  We piggy-back on the same SKIP_MBOX define
     * the QEMU CFLAGS already sets — if there's no VC mailbox,
     * there's no HDMI either, so the diagnostic is moot anyway. */
    return;
#endif

    /* 16 candidates across the low 4 GiB.  Stops at 0xF8000000 to
     * keep clear of any peripheral / firmware-reserved areas the
     * Pi 5 might map between 0xFC000000 and 0xFFFFFFFF.
     *
     * Colours are chosen distinct enough to identify by name on a
     * normal monitor: tell us which colour fills the screen and we
     * know the FB base to within ~256 MiB. */
    static const struct {
        unsigned long addr;
        unsigned int  colour;
    } candidates[] = {
        { 0x10000000UL, 0xFFFF0000U },  /* 0.25 GiB — RED            */
        { 0x20000000UL, 0xFFFF8000U },  /* 0.50 GiB — ORANGE         */
        { 0x30000000UL, 0xFFFFFF00U },  /* 0.75 GiB — YELLOW         */
        { 0x40000000UL, 0xFF80FF00U },  /* 1.00 GiB — LIME           */
        { 0x50000000UL, 0xFF00FF00U },  /* 1.25 GiB — GREEN          */
        { 0x60000000UL, 0xFF00FF80U },  /* 1.50 GiB — TEAL           */
        { 0x70000000UL, 0xFF00FFFFU },  /* 1.75 GiB — CYAN           */
        { 0x80000000UL, 0xFF0080FFU },  /* 2.00 GiB — SKY            */
        { 0x90000000UL, 0xFF0000FFU },  /* 2.25 GiB — BLUE           */
        { 0xA0000000UL, 0xFF8000FFU },  /* 2.50 GiB — PURPLE         */
        { 0xB0000000UL, 0xFFFF00FFU },  /* 2.75 GiB — MAGENTA        */
        { 0xC0000000UL, 0xFFFF80FFU },  /* 3.00 GiB — PINK           */
        { 0xD0000000UL, 0xFFFFFFFFU },  /* 3.25 GiB — WHITE          */
        { 0xE0000000UL, 0xFF808080U },  /* 3.50 GiB — GREY           */
        { 0xF0000000UL, 0xFF800000U },  /* 3.75 GiB — DARK RED       */
        { 0xF8000000UL, 0xFF008000U },  /* 3.875 GiB — DARK GREEN    */
    };

    int n = (int)(sizeof(candidates) / sizeof(candidates[0]));
    for (int i = 0; i < n; i++) {
        paint_contiguous(candidates[i].addr, candidates[i].colour);
    }
}
