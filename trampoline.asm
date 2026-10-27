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

    ; ---- params pointer ----
    mov  rcx, rsi              ; params pointer
    mov  eax, [rcx + 88]       ; firmware FV base (32-bit)
    mov  rbp, rax
    mov  eax, [rcx + 92]       ; firmware stack top (32-bit)
    mov  rsp, rax
    and  rsp, -16

    ; ---- compute trampoline runtime base ----
    call .Lbase
.Lbase:
    pop  rax
    sub  rax, .Lbase - start
    mov  rbx, rax               ; rbx = trampoline runtime base

    ; ---- build 256-entry IDT with ignore-interrupt handler ----
    mov  eax, [rcx + 96]       ; idt base (32-bit)
    mov  rdx, rax
    mov  r11, rbx
    add  r11, ign_intr - start  ; r11 = handler addr
    mov  r9, 256
.idt_fill:
    mov  r10, r11
    mov  [rdx], r10w
    mov  word [rdx + 2], 0x08
    mov  byte [rdx + 4], 0
    mov  byte [rdx + 5], 0x8E
    shr  r10, 16
    mov  [rdx + 6], r10w
    shr  r10, 16
    mov  [rdx + 8], r10d
    mov  dword [rdx + 12], 0
    add  rdx, 16
    dec  r9
    jnz  .idt_fill

    ; ---- load IDT ----
    mov  eax, [rcx + 100]      ; idtr base (32-bit)
    mov  rdx, rax
    mov  word [rdx], 0x0FFF
    mov  eax, [rcx + 96]       ; idt base (32-bit)
    mov  [rdx + 2], rax
    lidt [rdx]
    cli

    ; emit 'A' immediately before calling SEC (pre-SEC probe)
    mov  dx, 0x3FD
.pa1:
    in   al, dx
    test al, 0x20
    jz   .pa1
    mov  al, 'A'
    mov  dx, 0x3F8
    out  dx, al

    cli

    ; probe: print the byte at 0x80B000 (OVMF64 TDX-detection flag) and the
    ; entry point, so we can tell which SEC path is taken.
    mov  dx, 0x3FD
.pb1:
    in   al, dx
    test al, 0x20
    jz   .pb1
    mov  al, 'B'
    mov  dx, 0x3F8
    out  dx, al
    mov  eax, 0x80B000
    mov  dx, 0x3FD
.pb2:
    in   al, dx
    test al, 0x20
    jz   .pb2
    mov  al, [0x80B000]
    add  al, '0'
    mov  dx, 0x3F8
    out  dx, al

    ; OVMF64 PlatformPei expects IA32_MTRR_DEF_TYPE with E=1, FE=0 (fixed
    ; range off), default type = write-back (6).  Mirror the reference
    ; EFI32Boot transition.asm CPU hygiene (clear CR0 NW/CD, CR4.PSE, disable
    ; fixed-range MTRR) before handing off to the inner SEC.
    mov  ecx, 0x2FF            ; IA32_MTRR_DEF_TYPE
    rdmsr
    mov  eax, 0x806            ; E(1)=1 | FE(1)=0 | Type=WB(6) -> 0x806
    mov  edx, 0
    wrmsr
    mov  ecx, 0x2FF
    rdmsr
    mov  al, 'n'
    mov  dx, 0x3F8
    out  dx, al

    mov  eax, [rsi + 84]       ; firmware SEC entry point
    mov  rcx, rbp              ; SEC entry expects rcx = FV base (rbp)
    mov  rdx, rsp              ; SEC entry expects rdx = temp RAM stack
    call rax                   ; does not return

    mov  dx, 0x3FD
.postS:
    in   al, dx
    test al, 0x20
    jz   .postS
    mov  al, 'S'
    mov  dx, 0x3F8
    out  dx, al

.frost:
    cli
    mov  dx, 0x3FD
.f1:
    in   al, dx
    test al, 0x20
    jz   .f1
    mov  al, 'R'
    mov  dx, 0x3F8
    out  dx, al
    hlt
    jmp .frost

ign_intr:
    iretq

end_of_trampoline:
