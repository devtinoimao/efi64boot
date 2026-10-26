; 64-bit payload entry stub — first bytes of the flat payload binary.
; RCX = PAYLOAD_PARAMS* (set by trampoline, MS x64 ABI since the
; payload is compiled with x86_64-w64-mingw32-gcc).
;
; SELF-RELOCATION (added after the "GDTR loaded 0/0" bug): mingw resolves
; cross-object externs (saved_gdtr32, ctx, svc_table32, ...) through
; `.refptr.` indirection slots whose contents are BASE-STRIPPED
; (0-relative) blob offsets, with DIR64 fixups emitted in a .reloc blob
; at blob offset 0x1000. We link with -nostdlib, so no CRT ever applies
; them: the C side then reads/writes LOW linear memory (linear 0xfe0!)
; while the asm side accesses [rip+...] inside this copied blob at
; region+0x9000. Result: the two sides never share memory — the thunk's
; lgdt [saved_gdtr32] loaded 0/0. Fix: on entry, walk the embedded .reloc
; and add the RUNTIME base to every DIR64 slot. Then C and asm address
; the same bytes.
;
; Reloc layout (validated against build/payload64.bin):
;   @RELOC_OFF: dw VirtualAddress (junk: 0xC0000000 — ignored!)
;               dw SizeOfBlock
;               w entries: type nibble 10 = DIR64, low 12 bits = the
;               blob-relative BYTE address of the pointer slot (matches
;               the flat BASE=0 payload64.ld layout directly, so we can
;               patch at blob_base + (entry & 0xFFF)).
;
; NOTE: mingw emits one block per section with DIR64 fixups (.text/.data
; — currently two blocks). The loop previously trusted the next block's
; SizeOfBlock: after the last block that read fell PAST the copied blob
; into AllocatePages staging garbage (bug #6) — a garbage block then
; corrupted the stub via add [r8+rax], r8 (crash at blob+0x1D/0x1E,
; RAX=junk bytes). Fix: the walk is bounded by reloc_end (ld: one past
; the .reloc section), loaded rip-relative like reloc_start so it
; self-adjusts to the runtime base with no fixup involved.

BITS 64
global _payload_entry
extern payload64_main
extern reloc_start                       ; ld: = ADDR(.reloc)
extern reloc_end                         ; ld: = end of the .reloc section

_payload_entry:
    ; do NOT clobber RCX (params). Uses rax, rdx, r8, r9, r10, r11 only.
    lea  r8, [rel _payload_entry]          ; runtime blob base (offset 0)
    lea  rdx, [rel reloc_start]            ; runtime address of .reloc blob
    lea  r11, [rel reloc_end]              ; runtime address PAST the walk area

.blocks:
    cmp  rdx, r11
    jae  .done                             ; past .reloc — no more blocks
    mov  eax, [rdx]                        ; VirtualAddress — junk, ignored
    mov  r9d, [rdx + 4]                    ; SizeOfBlock
    test r9d, r9d
    jz   .done                             ; zero size = reloc end
    cmp  r9d, 8
    jb   .done                             ; malformed — stop
    add  rdx, 8
    sub  r9d, 8
    shr  r9d, 1                            ; entry count
.entries:
    movzx eax, word [rdx]                  ; entry = type<<12 | offs
    mov  r10d, eax
    shr  r10d, 12                          ; type (10 = DIR64)
    and  eax, 0x0FFF                       ; blob-relative offset
    cmp  r10d, 10
    jne  .skip                             ; padding/type0/other — ignore
    add  [r8 + rax], r8                    ; slot = blob_base + off; += base
.skip:
    add  rdx, 2
    dec  r9d
    jnz  .entries
    jmp  .blocks

.done:
    jmp payload64_main

section .note.GNU-stack noalloc noexec nowrite progbits
