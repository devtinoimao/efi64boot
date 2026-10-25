/* Flat x86_64 payload: everything consecutive, entry stub first. */
ENTRY(_payload_entry)
SECTIONS
{
    . = 0x0;
    .text   : { *(.text) *(.text.*) }
    .rodata : { *(.rodata*) *(.rdata*) }
    .data   : { *(.data*) }
    /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
}
