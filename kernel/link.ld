/* kernel/link.ld — AArch64 kernel image layout for Raspberry Pi 5.
 *
 * The Pi 5 firmware (after reading kernel_2712.img per config.txt) loads
 * the flat image at physical address 0x80000 and jumps to the first
 * instruction.  We must therefore put .text.boot's first instruction
 * exactly there.
 *
 * BSS is NOLOAD so it's not stored in the image file (saves SD space)
 * — boot.S zeroes it before calling C.
 *
 * __bss_size is in 8-byte units to match boot.S's `str xzr, [x1], #8`
 * loop body.
 */
SECTIONS
{
    . = 0x80000;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.* .gnu.linkonce.t*)
    }

    .rodata : { *(.rodata .rodata.* .gnu.linkonce.r*) }

    PROVIDE(_data = .);
    .data : { *(.data .data.* .gnu.linkonce.d*) }

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    _end = .;

    /DISCARD/ : { *(.comment) *(.gnu*) *(.note*) *(.eh_frame*) }
}

/* boot.S zeroes the BSS 8 bytes at a time; advertise the size in
 * 8-byte units to match its loop. */
__bss_size = (__bss_end - __bss_start) >> 3;
