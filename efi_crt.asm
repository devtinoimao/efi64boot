; EFI IA32 application entry: mingw calls efi_start with
; (ECX = image handle?, ...). Actually UEFI IA32 passes args on the
; stack per cdecl: (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable).
; We forward to efi_main.

BITS 32
section .text
global efi_start
extern _efi_main

efi_start:
    push ebp
    mov  ebp, esp
    push dword [ebp + 12]      ; SystemTable
    push dword [ebp + 8]       ; ImageHandle
    call _efi_main
    leave
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
