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

    ; ---- M2: capture 32-bit firmware state into BOOT_PARAMS ------
    ; MUST happen BEFORE we lgdt the bridge GDT below — the whole point
    ; is to save the FIRMWARE GDT/IDT/CRs/segments, and at this point
    ; the GDTR still holds the firmware tables. (Earlier this block sat
    ; after the bridge lgdt, so saved_gdtr32 ended up being the bridge
    ; GDT and the thunk's far return to the 32-bit CS was unresolvable:
    ; #GP at the lret, double fault, reset.)
    ;
    ; sgdt in 32-bit mode writes a packed 6-byte blob: limit(2)+base(4).
    ; BOOT_PARAMS offsets 24.. :
    ;   +24  gdtr blob (6 bytes)   +32  idtr blob (6 bytes)
    ;   +40 cr0  +44 cr3  +48 cr4
    ;   +52 cs  +56 ds  +60 es  +64 ss  +68 fs  +72 gs  +76 esp
    sgdt [esi + 24]
    sidt [esi + 32]
    mov  eax, cr0
    mov  [esi + 40], eax
    mov  eax, cr3
    mov  [esi + 44], eax
    mov  eax, cr4
    mov  [esi + 48], eax
    mov  ax, cs
    mov  [esi + 52], eax
    mov  ax, ds
    mov  [esi + 56], eax
    mov  ax, es
    mov  [esi + 60], eax
    mov  ax, ss
    mov  [esi + 64], eax
    mov  ax, fs
    mov  [esi + 68], eax
    mov  ax, gs
    mov  [esi + 72], eax
    mov  [esi + 76], esp

    ; ---- runtime GDT descriptor on the stack --------------------
    ; [ebp-8] = {word limit, dword base}; use eax scratch
    mov  eax, [esi + 16]       ; gdt_phys
    sub  esp, 8
    mov  word [esp], 0x27      ; limit = 39 (5 entries: null, code64,
                               ; data, 32-bit compat code @0x18, and
                               ; another 32-bit code @0x20 for the
                               ; in_compat -> in_32bit 4+4 retf)
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
