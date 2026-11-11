/*
 * PAPP v2 linker script.
 *
 * The image is linked at address 0 and relocated by the loader to wherever it
 * lands (papp_pack.py turns the --emit-relocs output into a relocation table).
 *
 * SLOW region (PSRAM), linked at 0:
 *   [ .text | .rodata | .init_array | .fini_array | .data ]  ← stored in file
 *   [ .bss ]                                                 ← zeroed by loader
 * FAST region (internal SRAM), linked at 0x10000000 so that nothing in one
 * region can reach the other with a short jump (jal/branch → link error):
 *   [ .fast (PAPP_FAST code, PAPP_FAST_DATA) ]               ← stored in file
 *   [ .fast_bss (PAPP_FAST_BSS) ]                            ← zeroed by loader
 */
OUTPUT_FORMAT("elf32-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_papp_start)

SECTIONS
{
    . = 0;

    .text : {
        KEEP(*(.text.papp_entry))
        *(.text .text.*)
    }

    .rodata : ALIGN(4) {
        KEEP(*(.papp_manifest))
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }

    .init_array : ALIGN(4) {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array .ctors))
        __init_array_end = .;
    }

    .fini_array : ALIGN(4) {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array .dtors))
        __fini_array_end = .;
    }

    .data : ALIGN(16) {
        __papp_imports_start = .;
        KEEP(*(.papp_imports))
        __papp_imports_end = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(4);
    }

    __papp_image_end = .;

    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(.ext_ram.bss .ext_ram.bss.*)                       /* ESP-IDF PSRAM bss: apps live in PSRAM anyway */
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    .fast 0x10000000 : ALIGN(16) {
        __fast_start = .;
        *(.fast.text .fast.text.*)
        *(.fast.rodata .fast.rodata.*)
        *(.fast.data .fast.data.*)
        . = ALIGN(16);
        __fast_image_end = .;
    }

    .fast_bss __fast_image_end (NOLOAD) : ALIGN(16) {       /* pinned: with an empty .fast it drifted out of the FAST region */
        *(.fast.bss .fast.bss.*)
        . = ALIGN(16);
        __fast_bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.riscv.attributes)
    }
}
