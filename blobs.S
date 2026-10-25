/* Embedded flat binaries: 32-bit trampoline + 64-bit payload. */

    .global _trampoline_start
    .global _trampoline_size
    .global _payload64_start
    .global _payload64_size

    .section .rdata, "dr"

_trampoline_start:
    .incbin "build/trampoline.bin"
_trampoline_end:
_trampoline_size:
    .long _trampoline_end - _trampoline_start

    .align 8
_payload64_start:
    .incbin "build/payload64.bin"
_payload64_end:
_payload64_size:
    .long _payload64_end - _payload64_start
