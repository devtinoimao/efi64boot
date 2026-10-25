; efi64boot trampoline: 32-bit protected mode -> long mode handoff.
; Assembled with `nasm -f bin` (flat, ORG 0) and embedded into the
; BOOTIA32.EFI image; copied at runtime to a staging page and entered
; there, so EVERYTHING here must be position-independent — no absolute
; address encodings. The far jump into 64-bit mode is synthesized at
; runtime via self-located base + retf.
;
; BOOT_PARAMS layout (uint32 each, from main.c):
;   +0  pml4_phys
;   +4  payload_phys
;   +8  payload_size
;   +12 stack_top
;   +16 gdt_phys
;   +20 serial_ok

BITS 32

%define CODE64_SEL 0x08

; entry: ESI = BOOT_PARAMS*  (set by caller; we clobber nothing needed)
start:
    mov  esi, [esp + 4]        ; cdecl first arg: BOOT_PARAMS* (the C
                               ; caller pushes it on the stack — ESI
                               ; held garbage otherwise: CR3 ended up
                               ; as firmware RaiseTPL ptr -> instant
                               ; page-fault loop once PG re-enabled)
    cli                        ; no interrupts across the mode switch

    ; ---- debug: 'p' marker helper inlined below ----

    ; ---- runtime GDT descriptor on the stack --------------------
    ; [ebp-8] = {word limit, dword base}; use eax scratch
    mov  eax, [esi + 16]       ; gdt_phys
    sub  esp, 8
    mov  word [esp], 0x17      ; limit = 23 (3 entries)
    mov  [esp + 2], eax        ; base
    lgdt [esp]
    add  esp, 8

    ; ---- enable PAE ---------------------------------------------
    mov  eax, cr4
    or   eax, 1 << 5
    mov  cr4, eax

    ; ---- CR3 = PML4 ---------------------------------------------
    mov  eax, [esi + 0]        ; pml4_phys
    mov  cr3, eax

    ; ---- EFER.LME = 1 -------------------------------------------
    mov  ecx, 0xC0000080       ; IA32_EFER
    rdmsr
    or   eax, 1 << 8           ; LME
    wrmsr

    ; ---- enable paging -------------------------------------------
    mov  eax, cr0
    or   eax, 0x80000000       ; PG (PE already set: we run from EFI)
    mov  cr0, eax

    ; debug: emit 'p' (still 32-bit, identity map must be live now)
    mov  dx, 0x3FD
.l1:
    in   al, dx
    test al, 0x20
    jz   .l1
    mov  al, 'p'
    mov  dx, 0x3F8
    out  dx, al

    ; ---- position-independent far jump into 64-bit ---------------
    call .next                 ; pushes EIP
.next:
    pop  eax                   ; eax = VA of .next
    sub  eax, .next - start    ; eax = trampoline base VA
    add  eax, lm_entry - start ; eax = VA of lm_entry
    push dword CODE64_SEL
    push eax
    retf                       ; far return: EIP=lm_entry, CS=CODE64_SEL

BITS 64
lm_entry:
    ; debug: emit 'T' — we are executing in 64-bit long mode
    mov  dx, 0x3FD
.t1:
    in   al, dx
    test al, 0x20
    jz   .t1
    mov  al, 'T'
    mov  dx, 0x3F8
    out  dx, al

    ; params pointer: ESI was set in 32-bit mode; 32-bit ops zero-extend
    ; into RSI on x86-64, and everything we allocate is below 4 GiB.
    mov  rcx, rsi              ; MS x64 ABI: first integer arg in RCX
    mov  esp, [rsi + 12]       ; stack_top — DWORD read: zero-extends into
    ; RSP (a qword read here would capture gdt_phys too — bug)

    and  rsp, -16

    mov  eax, [rsi + 4]        ; payload_phys — DWORD read (see above)
    call rax                   ; payload64_entry(params)

.frost:                        ; not reached in M1
    cli
    hlt
    jmp .frost

end_of_trampoline:
