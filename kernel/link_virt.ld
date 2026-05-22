/* kernel/link_virt.ld — QEMU `-M virt` variant of link.ld.
 *
 * QEMU's aarch64 virt machine starts RAM at 0x40000000 and the
 * generic AArch64 -kernel loader places flat binaries at
 * RAM_base + 0x80000 = 0x40080000.  Everything else (BSS NOLOAD,
 * __bss_size in 8-byte units) matches the Pi 5 link script —
 * only the load address differs.
 */
SECTIONS
{
    . = 0x40080000;

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

__bss_size = (__bss_end - __bss_start) >> 3;
