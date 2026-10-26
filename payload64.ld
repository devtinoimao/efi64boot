/* Flat x86_64 payload: everything consecutive, entry stub first. */
ENTRY(_payload_entry)
BASE = 0x0;
SECTIONS
{
    . = BASE;
    .text   : { *(.text) *(.text.*) }
    .rodata : { *(.rodata*) *(.rdata*) }
    .data   : { *(.data*) *(.bss*) *(COMMON) }
    /* Keep the mingw .reloc blob findable from payload64_entry.asm's
       self-relocator regardless of where .data happens to end. */
    . = ALIGN(0x1000);
    .reloc  : { *(.reloc*) }
    reloc_start = ADDR(.reloc);
    reloc_end   = .;         /* section end — self-relocator walk bound */
    /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
