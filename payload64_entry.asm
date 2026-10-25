; 64-bit payload entry stub — first bytes of the flat payload binary.
; RCX = PAYLOAD_PARAMS* (set by trampoline, MS x64 ABI since the
; payload is compiled with x86_64-w64-mingw32-gcc).

BITS 64
global _payload_entry
extern payload64_main

_payload_entry:
    jmp payload64_main

section .note.GNU-stack noalloc noexec nowrite progbits
