; efi64boot M2 — 64->32 service thunks (v2, bridge-context design).
;
; All cross-mode state flows through BRIDGE_CTX (in .data, linked into the
; 64-bit payload blob at a <4GiB address). Nothing survives the round trip
; in registers except the final result read back from the context.
;
; Firmware services return EFI_STATUS as EDX:EAX on IA32 (learned in M1:
; a 64-bit status IS returned in edx:eax) — we capture both halves.

BITS 64

global saved_gdtr32, saved_idtr32
extern saved_cr0_32, saved_cr3_32, saved_cr4_32
extern saved_ds32, saved_es32, saved_fs32, saved_gs32, saved_ss32, saved_cs32
extern thunk_stack32_top
extern svc_table32         ; u32 array: 32-bit service entry points

global do_thunk_call

%define CODE64_SEL 0x08

; ---- serial probe macros (DEBUG ONLY — delete once the round trip works) ----
; MS x64 caller-saved regs only (r11b, al, dx), so the C caller is unaffected.
%macro MP_PUT 0
    mov  dx, 0x3FD               ; COM1 LSR: wait for transmitter empty
%%wait:
    in   al, dx
    test al, 0x20
    jz   %%wait
    mov  dx, 0x3F8
    mov  al, r11b
    out  dx, al
%endmacro

%macro MP_CHAR 1
    mov  r11b, %1
    MP_PUT
%endmacro

%macro MP_HEX2 0            ; print r11b as two hex chars (clobbers al, r10)
    push r11
    lea  r10, [rel hexchars]
    mov  al, r11b
    shr  al, 4
    and  eax, 0x0F
    mov  r11b, [r10 + rax]
    MP_PUT
    pop  r11                ; original byte back in r11b
    mov  al, r11b
    and  eax, 0x0F
    mov  r11b, [r10 + rax]
    MP_PUT
%endmacro

%macro MP_DUMP 0            ; print 8 bytes from probe_gdtr64 as hex
    lea  r8, [rel probe_gdtr64]
    mov  ecx, 8
%%again:
    mov  r11b, [r8]
    MP_HEX2
    inc  r8
    dec  ecx
    jnz  %%again
    MP_CHAR '|'
%endmacro

%macro MP_DUMP2 0           ; print bytes 8..15 of probe_gdtr64 as hex
    lea  r8, [rel probe_gdtr64 + 8]
    mov  ecx, 8
%%again:
    mov  r11b, [r8]
    MP_HEX2
    inc  r8
    dec  ecx
    jnz  %%again
    MP_CHAR '|'
%endmacro

; sgdt/sidt the live table and print %2 if the limit's low byte matches %1,
; else '.'. 1200-baud proof of what the CPU actually holds right now.
%macro MP_PROBE_GDT 2
    sgdt [rel probe_gdtr64]
    mov  r11b, '.'
    cmp  byte [rel probe_gdtr64], %1
    jne  %%no
    mov  r11b, %2
%%no:
    MP_PUT
%endmacro

%macro MP_PROBE_IDT 2
    sidt [rel probe_gdtr64]
    mov  r11b, '.'
    cmp  byte [rel probe_gdtr64], %1
    jne  %%no
    mov  r11b, %2
%%no:
    MP_PUT
%endmacro

; ---- compat-mode probe variants (WARN: executing in a D=1 32-bit code
; segment — 32-bit decode has NO REX prefixes (r8-r15 unusable) and NO
; RIP-relative addressing, so everything is ebp-based, eax/esi/edi only.
; EBP must already hold the runtime VA of in_compat.) ----
%macro CP_PUT 0
    push eax               ; preserve the char in AL across the LSR poll
%%wait:
    mov  dx, 0x3FD
    in   al, dx
    test al, 0x20
    jz   %%wait
    pop  eax               ; restore the char to AL
    mov  dx, 0x3F8
    out  dx, al
%endmacro

%macro CP_CHAR 1
    mov  al, %1
    CP_PUT
%endmacro

%macro CP_DUMP 0           ; print 8 bytes of probe_gdtr64 as hex
    lea  esi, [ebp + probe_gdtr64 - in_compat]
    lea  edi, [ebp + hexchars - in_compat]
    mov  ecx, 8
%%again:
    mov  al, [esi]
    shr  al, 4
    and  eax, 0x0F
    mov  al, [edi + eax]
    CP_PUT
    mov  al, [esi]
    and  eax, 0x0F
    mov  al, [edi + eax]
    CP_PUT
    inc  esi
    dec  ecx
    jnz  %%again
    CP_CHAR '|'
%endmacro

struc BRIDGE
    .svc_entry:   resd 1        ; 32-bit service entry point
    .nargs:       resd 1
    .args:        resd 8        ; 32-bit arg values
    .ret_lo:      resd 1        ; EAX
    .ret_hi:      resd 1        ; EDX
    .scratch_top: resd 1        ; 32-bit stack top for the call
    .rflags:      resq 1
    .rsp:         resq 1
    .cr0:         resq 1
    .cr3:         resq 1
    .cr4:         resq 1
    .efer:        resd 2
    .gdtr:        resq 2
    .idtr:        resq 2
    .cs:          resd 1
    .ss:          resd 1
    .ds:          resd 1
    .es:          resd 1
endstruc

section .text

; do_thunk_call(u64 *args64 (rcx), u64 *retout (rdx), u32 svc_index (r8d),
;               u32 nargs (r9d))  -- MS x64 ABI (mingw)
; Returns 64-bit EFI_STATUS in rax. All pointers < 4 GiB.
do_thunk_call:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    sub  rsp, 0x40

    mov  r14, rdx                  ; retout (64-bit ptr)
    mov  r10d, r8d                 ; svc index
    mov  r11d, r9d                 ; nargs
    ; ---- fill bridge context (64-bit side) ----
    lea  r13, [rel ctx]
    mov  eax, [rel svc_table32]
    add  eax, r10d
    mov  eax, [eax]
    mov  [r13 + BRIDGE.svc_entry], eax
    mov  [r13 + BRIDGE.nargs], r11d
    xor  r8d, r8d
.fill_args:
    cmp  r8d, r11d
    jae  .args_filled
    mov  rax, [rcx + r8*8]
    mov  [r13 + BRIDGE.args + r8*4], eax
    inc  r8d
    jmp  .fill_args
.args_filled:
    mov  rax, [rel thunk_stack32_top]
    mov  [rel ctx + BRIDGE.scratch_top], eax

    ; ---- save 64-bit state into ctx ----
    mov  rax, cr0
    mov  [rel ctx + BRIDGE.cr0], rax
    mov  rax, cr3
    mov  [rel ctx + BRIDGE.cr3], rax
    mov  rax, cr4
    mov  [rel ctx + BRIDGE.cr4], rax
    mov  ecx, 0xC0000080
    rdmsr
    mov  [rel ctx + BRIDGE.efer], eax
    mov  [rel ctx + BRIDGE.efer + 4], edx
    sgdt [rel ctx + BRIDGE.gdtr]
    sidt [rel ctx + BRIDGE.idtr]
    pushfq
    pop  rax
    mov  [rel ctx + BRIDGE.rflags], rax
    mov  [rel ctx + BRIDGE.rsp], rsp
    mov  [rel ctx + BRIDGE.cs], cs
    mov  [rel ctx + BRIDGE.ss], ss
    mov  [rel ctx + BRIDGE.ds], ds
    mov  [rel ctx + BRIDGE.es], es

    cli

    ; DEBUG probe: dump live GDTR before we switch (expect bridge GDT:
    ; 17 00 xx xx xx xx 00 00)
    sgdt [rel probe_gdtr64]
    MP_CHAR 'X'
    MP_DUMP

    ; DEBUG probe 'G' (deletable): bridge GDT entry 0x18 — must be the
    ; compat descriptor 00 CF 9A 00 00 00 FF FF → bytes 00 CF 9A 00 00
    ; 00 FF FF hmm — little-endian memory view: FF FF 00 00 00 9A CF 00
    sgdt [rel probe_gdtr64]
    mov  rax, [rel probe_gdtr64 + 2]    ; GDT base (bytes 2..9)
    mov  rax, [rax + 0x18]
    mov  [rel probe_gdtr64], rax
    MP_CHAR 'G'
    MP_DUMP

    ; ---- exit long mode via the compat-mode dance (plan B2) ----
    ; B1 FAILURES (empirical, do not retry):
    ;   * two 8-byte pushes + bare `cb`: 64-bit far ret popped 8+8, CS
    ;     taken from the high half of the EIP slot → CS=0 → #GP(0).
    ;   * `db 0x66; retf` after PG-clear: QEMU decodes 16-bit default
    ;     (bridge code64 descriptor is L=1, D=0) and the 0x66 prefix had
    ;     NO EFFECT on the far ret → it still popped 2+2, CS = high 16
    ;     bits of the EIP slot (0x7EAD → #GP e=0x7EAC).
    ; B2: far-return LONG → 32-bit COMPAT code segment (selector 0x18 in
    ; the bridge GDT) FIRST, while PG is still on; every subsequent step
    ; runs with a D=1 cached descriptor where near/far rets decode
    ; unambiguously as 32-bit.
    mov  eax, 0x18                 ; compat code selector (bridge GDT)
    push rax                       ; CS slot (8 bytes)
    lea  rax, [rel in_compat]
    push rax                       ; RIP slot (8 bytes)
    ; DEBUG probe 'P' (deletable): the far return frame just built:
    ; expect <low8 of RIP...> = 15 28 AD 7E 00 00 00 00 and CS 18 00 00
    ; 00 00 00 00 00
    mov  rax, [rsp]
    mov  [rel probe_gdtr64], rax
    mov  rax, [rsp + 8]
    mov  [rel probe_gdtr64 + 8], rax
    MP_CHAR 'P'
    MP_DUMP
    MP_DUMP2
    ; DEBUG probe 'U' (deletable): final frame re-check right before
    ; the retf — prints [rsp] and [rsp+8] a second time and GDT[0x18]
    mov  rax, [rsp]
    mov  [rel probe_gdtr64], rax
    mov  rax, [rsp + 8]
    mov  [rel probe_gdtr64 + 8], rax
    mov  r10, rax                        ; keep CS slot in r10 too
    MP_CHAR 'U'
    MP_DUMP
    MP_DUMP2
    mov  rax, r10
    ; jmp far [rsp] = m16:64: 8-byte offset + 2-byte selector at [rsp].
    ; [RES]  The retf that previously sat here deterministically raised
    ; #GP e=0000 in the OVMF32 context while an identical state works
    ; standalone (farret.img/farret2.img round trips — and farret2 now
    ; proves -smp 2 does NOT matter). efi32boot's proven transition.asm
    ; uses exactly this far-jmp form for long→compat for the same
    ; reason ("a retf ... faults instead" — their empirical note), so
    ; the frame (RIP64@[rsp], sel16@[rsp+8]) simply stays put.
    jmp  far [rsp]                 ; 64-bit far jmp: CS <- 0x18 compat

BITS 32
in_compat:
    ; D=1 compat mode, PG still ON (identity map active), bridge GDT
    ; loaded. 32-bit decode: r8-r15 unusable, RIP-relative invalid;
    ; ebp holds the runtime VA of in_compat (self-located below).
    call .self
.self:
    pop  ebp
    sub  ebp, .self - in_compat

    ; DEBUG probe (deletable): GDTR must still be the bridge GDT here
    sgdt [ebp + probe_gdtr64 - in_compat]
    CP_CHAR 'C'
    CP_DUMP

    ; Firmware IDT: keep the 32-bit IDT loaded for the service call.
    ; (We deliberately do NOT lgdt the firmware GDT here — the bridge GDT
    ; stays active so the 4+4 retf below can target our own 0x20 code32
    ; descriptor. Loading the firmware GDT was the #GP e=0x0010 source:
    ; its 0x10 selector is a DATA segment, not code.)
    lidt [ebp + saved_idtr32 - in_compat]
    ; DEBUG probe (deletable): expect the firmware IDT
    ; (FF 07 10 D0 82 7F xx xx)
    sgdt [ebp + probe_gdtr64 - in_compat]
    CP_CHAR 'F'
    CP_DUMP

    ; We're already executing in a D=1 compat code segment (CS=0x18), so
    ; there is no need to clear PG / do a 4+4 retf. Just jump to in_32bit
    ; directly; the Stall service will run with its own normal CR0 state.
    jmp  in_32bit

BITS 32
in_32bit:
    ; self-locate: EBP = runtime VA of in_32bit (blob is position-
    ; independent; ctx/saved_* are reached as EBP + link-time offsets)
    call .self
.self:
    pop  ebp
    sub  ebp, .self - in_32bit

    ; DEBUG probe: 'I' = we landed in in_32bit after the 4+4 retf
    CP_CHAR 'I'

    ; Set flat bridge data segment for the 32-bit environment.
    CP_CHAR 'A'
    ; Set flat bridge data for in_32bit.
    CP_CHAR 'A'
    mov  ax, 0x10
    mov  ds, ax
    mov  ss, ax
    mov  es, ax
    CP_CHAR 'B'
    mov  esp, [ebp + ctx - in_32bit + BRIDGE.scratch_top]
    CP_CHAR 'X'
    ; cdecl: push args in reverse
    mov  ecx, [ebp + ctx - in_32bit + BRIDGE.nargs]
.push_args:
    test ecx, ecx
    jz   .args_done
    dec  ecx
    mov  eax, [ebp + ctx - in_32bit + BRIDGE.args + ecx*4]
    push eax
    jmp  .push_args
.args_done:
    CP_CHAR 'C'
    mov  eax, [ebp + ctx - in_32bit + BRIDGE.svc_entry]
    CP_CHAR 'P'
    call eax
    CP_CHAR 'Q'
    ; firmware returns EFI_STATUS in EDX:EAX (M1 lesson)
    mov  [ebp + ctx - in_32bit + BRIDGE.ret_lo], eax
    mov  [ebp + ctx - in_32bit + BRIDGE.ret_hi], edx
    CP_CHAR 'S'
    CP_CHAR 'L'

    ; ---- re-enter long mode ----
    ; We must load the 64-bit GDT/IDT BEFORE the far ret, because the
    ; active GDT right now is the 32-bit firmware GDT (saved_gdtr32),
    ; whose descriptor for CODE64_SEL is NOT a 64-bit code segment.
    ; The 64-bit GDT that owns CODE64_SEL is saved in ctx (bridge GDT).
    sub  esp, 16
    mov  eax, [ebp + ctx - in_32bit + BRIDGE.gdtr]        ; limit(16)
    mov  ecx, [ebp + ctx - in_32bit + BRIDGE.gdtr + 2]    ; base(32)
    mov  word [esp], ax
    mov  [esp + 2], ecx
    mov  eax, [ebp + ctx - in_32bit + BRIDGE.idtr]        ; limit(16)
    mov  ecx, [ebp + ctx - in_32bit + BRIDGE.idtr + 2]    ; base(32)
    mov  word [esp + 8], ax
    mov  [esp + 10], ecx
    lgdt [esp]
    lidt [esp + 8]
    add  esp, 16
    ; CR4.PAE was never cleared; EFER.LME still set.
    mov  eax, cr0
    or   eax, 0x80000000
    mov  cr0, eax
    push dword CODE64_SEL
    ; BUG C: `lea eax,[back_in_64]` is an ABSOLUTE link-time address
    ; (0x2000xxxx ≈ 8.5GiB linear — not in our [0,4GiB) identity map).
    ; Self-located ebp gives the pi-relative offset instead.
    lea  eax, [ebp + back_in_64 - in_32bit]
    push eax
    retf

BITS 64
back_in_64:
    lgdt [rel ctx + BRIDGE.gdtr]
    lidt [rel ctx + BRIDGE.idtr]
    mov  rax, [rel ctx + BRIDGE.cr4]
    mov  cr4, rax
    mov  rax, [rel ctx + BRIDGE.cr3]
    mov  cr3, rax
    mov  rax, [rel ctx + BRIDGE.cr0]
    mov  cr0, rax
    mov  ecx, 0xC0000080
    mov  eax, [rel ctx + BRIDGE.efer]
    mov  edx, [rel ctx + BRIDGE.efer + 4]
    wrmsr
    mov  rsp, [rel ctx + BRIDGE.rsp]

    ; status: sign-extend 32-bit EFI_STATUS to 64-bit
    mov  eax, [rel ctx + BRIDGE.ret_lo]
    movsxd rax, eax
    push rax
    mov  rax, [rel ctx + BRIDGE.rflags]
    push rax
    popfq
    pop  rax
    ; ES/DS: restore 64-bit view (load null data selector? our 64-bit
    ; code uses flat firmware data cached — restore saved values)
    mov  es, [rel ctx + BRIDGE.es]
    mov  ds, [rel ctx + BRIDGE.ds]

    add  rsp, 0x40
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbp
    pop  rbx
    ret

section .bss
alignb 8
saved_gdtr32: resb 10        ; 64-bit GDTR form: limit(2) + base(8)
saved_idtr32: resb 10

section .data
global ctx
ctx:
    istruc BRIDGE
    at BRIDGE.svc_entry,   dd 0
    at BRIDGE.nargs,       dd 0
    at BRIDGE.args,        times 8 dd 0
    at BRIDGE.ret_lo,      dd 0
    at BRIDGE.ret_hi,      dd 0
    at BRIDGE.scratch_top, dd 0
    at BRIDGE.rflags,      dq 0
    at BRIDGE.rsp,         dq 0
    at BRIDGE.cr0,         dq 0
    at BRIDGE.cr3,         dq 0
    at BRIDGE.cr4,         dq 0
    at BRIDGE.efer,        dd 0, 0
    at BRIDGE.gdtr,        dq 0, 0
    at BRIDGE.idtr,        dq 0, 0
    at BRIDGE.cs,          dd 0
    at BRIDGE.ss,          dd 0
    at BRIDGE.ds,          dd 0
    at BRIDGE.es,          dd 0
    iend

; DEBUG probe buffer for MP_PROBE_* (sgdt/sidt write 10 bytes)
align 8
probe_gdtr64: dq 0, 0
hexchars:     db "0123456789ABCDEF"

; The 32-bit half uses ABSOLUTE linear addresses of ctx/saved_* (they are
; all < 4 GiB by construction). We emit placeholder 32-bit absolute refs
; that the linker resolves via a small glue object — see thunk32_glue.
; For simplicity we reference them via two global label pairs:
extern saved_ds32, saved_ss32, saved_es32

section .note.GNU-stack noalloc noexec nowrite progbits
