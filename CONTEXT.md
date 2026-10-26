# efi64boot continuation context — UPDATED 2026-10-26 (handoff doc, v4)

## MISSION (user directives)
- Finish **efi64boot**: 32-bit BOOTIA32.EFI that launches 64-bit code. M2 = working
  64↔32 service-call thunk round-trip (`Stall(1000000)` through `do_thunk_call`),
  then advance to booting a REAL 64-bit Windows image (/mnt/hdd2/Images, prefer
  win10ltsc.iso).
- Keep both projects AI-assisted & QEMU-tested, explicitly not production-safe.
- Preserve AI-assisted-development disclosure for public-facing work.
- efi32boot is PAUSED/stable — do NOT work on it.
- PENDING USER QUESTION (unanswered): "did u use the code from the zcode session?"
  → factually NO session data was ever read directly; still should check
  ~/.zcode/cli/rollout/*.jsonl for a concurrent ZCode session before more edits.

## ★ CURRENT MILESTONE STATUS (v4)
- M1 (64-bit payload boots, HELLO, self-reloc, svc binding): **DONE & HEALED**.
- Bug #6 (reloc-walk overrun) FIXED. Boot fully healed (m2_t22/m2_int22).
- M2 (thunk round-trip): **DOWN-LEG SOLVED — `retf` replaced by `jmp far [rsp]`**
  (efi32boot's proven form). Far-jmp WORKED: execution enters compat (CS=0x18),
  the C probe fires. **REMAINING BLOCKER: #GP e=0010 at blob+0xBD5** — the
  `lea eax,[ebp+0x10F]` immediately AFTER `mov cr0,eax` clears PG inside
  in_compat (see NEW FAULT section). Up-path (in_32bit) never reached yet.
- **FAULT PRECISELY RESOLVED (byte-decoded)**: blob[0xBD0:0xBDF] = `00 89 44
  24 04 cb ...` → the byte AT 0xBD5 is **`cb` = the 4+4 retf itself** (not the
  LEA — earlier mis-attribution). So the fault is ON the compat→32-bit far
  return; RAX=0x10 at fault proves `mov eax,[ebp+0x7BD]` (saved_cs32) already
  ran. #GP e=0010 = new_cs&0xfffc of the RETFed selector 0x10 → **loading
  selector 0x10 into CS failed in helper_ret_protected (shift=1)**.
- All ebp-relative data offsets in the compat section VERIFIED CORRECT via nm:
  saved_gdtr32=0x12C0 (lgdt disp 0x7F9 ✓), saved_idtr32=0x12CA (0x803 ✓),
  probe_gdtr64=0x1118 (0x651 ✓), hexchars=0x1128 (0x661 ✓), in_32bit=0xBD6
  (0x10F ✓), saved_cs32=0x1284 (0x7BD ✓). NOTE: earlier "probe_gdtr64=
  blob+0x1128" reading was WRONG — 0x1128 is hexchars.
- QEMU retf check chain (seg_helper.c:1898, verified): GP(0x10) can only come
  from: null-sel no / load_segment fail / !(e2&S)||!(e2&CS) / rpl<cpl / non-
  conforming dpl!=rpl. NP would be 0x0B (not ours). Firmware 0x10 in the
  FIRMWARE GDT IS a valid code32 DPL0 ([CRA] per QEMU dump) → retf would be
  legal ⇒ **conclusion: at retf time the ACTIVE GDT ≠ firmware GDT** — i.e.
  either lgdt never executed (F probe absence supports this!) or saved_gdtr32
  content at runtime ≠ firmware GDTR → bridge GDT still loaded → its 0x10 is
  DATA desc → GP(0x10) EXACTLY AS OBSERVED.
- C fill code verified (payload64.c ~line 105): saved_gdtr32[0..5] ← params+24
  blob bytes 0..5 (=limit+base of firmware GDTR, should be 47 00 00 F0 C3 7F).
- Backtick mystery SOLVED ( theory to confirm): CP_PUT likely clobbers AL with
  the LSR value from `in al,dx` before `out dx,al` → every compat char prints
  as LSR≈0x60 = '`'. If so, the 'C'/'F' probes MAY have run (as backticks) —
  count the backticks in m2_t25.log to deduce which probes fired. MP prints
  were correct all along → check whether MP_PUT polls differently than CP_PUT.
- Working theories ranked: (1) lgdt/lidt never executed (blocked earlier) →
  bisect with a probe letter between them via the C-probe → F-probe gap;
  (2) saved_gdtr32 runtime content wrong/clobbered → verify via the C debug
  print in m2_t25.log line "saved_gdtr32      : " (expect 47 00 00 f0 c3 7f
  00 00 00 00); (3) QEMU e=0x10 oddity on cr0-write validation — now DEMOTED
  (fault PC = the retf, not mov cr0; v8.2 misc_helper.c fetched — NO
  helper_write_cr0 in it, lives elsewhere, fetch target/i386/helper.c if
  still needed).
- farret2 mimic (payload-state standalone): **PASSING** (`R G M O K X B R C`,
  zero exceptions) — REQUIRES `-m 2048` (NOT -m 128: the high addresses are
  unassigned phys under 128 MiB; earlier "#UD at RIP=3" was that artifact, NOT
  a real failure). Passes under BOTH -smp 1 and -smp 2.

## LANDED THIS SESSION (do not redo)
1. **BUG #6 FIXED: self-relocator walked past .reloc into staging garbage.**
   .reloc contains TWO blocks: block1 @0x2000 size 0x1C (9 DIR64 u16 entries,
   offsets 0xF70..0xFFF), block2 @0x201C size 0x10 (3 entries 0x1000/0x1010/0x1020)
   — and .reloc ends EXACTLY at 0x202C = copy end. After block2 the walk read
   SizeOfBlock one past the copied blob → AllocatePages leftovers (RAX=0x45 seen
   at fault) → treated as a block → stray `add [r8+rax],r8` corrupted the stub's
   own `jb` opcode at blob+0x1D → mid-instruction fetch #GP e=0x10 at blob+0x1E.
   **Fix:** `reloc_end = .;` exported from payload64.ld (right after the .reloc
   section), extern'd in payload64_entry.asm, `lea r11,[rel reloc_end]`, bound
   `cmp rdx,r11 / jae .done` at `.blocks`. Rip-relative lea = self-adjusting,
   NO new fixups (verified .reloc byte-identical after rebuild; mingw resolves
   the lea statically, same as reloc_start's). RCX (params) untouched; use r11.
   Entry format (validated): u16 entries, type nibble top-4 (0xA=DIR64),
   offset low-12 = blob-relative slot address; walk counts (SizeOfBlock-8)/2.
2. Verified healed boot (esp_m9.img, m2_t22/m2_int22): HELLO prints, ~17 boots
   in 45 s, EVERY boot crashes IDENTICALLY at the retf (all 2676 v-lines in
   m2_int22 are v=0d e=0000 at the same RIP). Deterministic, not flaky.

## VERIFICATION DATA (m2_t22, one boot state)
- Region=0x7EAC9000 → payload=0x7EAD2000; GDT(btg)=0x7EACF000 limit 0x1F;
  in_compat=blob+0x2AC4; retf=blob+0x2AC3; probe_gdtr64=blob+0x1128; RSP=0x7EAE7EA8.
- Probe chain at retf: `X1F0000F0AC7E0000|` (GDTR=0x7EACF000/0x1F ✓),
  `GFFFF0000009ACF00|` (GDT[0x18]=0x00CF9A000000FFFF ✓),
  `PC42AAD7E00000000|1800000000000000|` + U re-read = **frame byte-perfect**:
  [rsp]=0x7EAD2AC4 (in_compat), [rsp+8]=0x18.
- Fault dump: CS=0008 00af9a00 CS64, all segs=0008 data 00cf9300, GDTR=bridge,
  IDT=firmware 0x7F82D010/0x7FF, CR0=0x80000033, CR3=0x7EAC9000, CR4=0x660,
  EFER=0x500, RFL=2, RSP=0x7EAE7EA8 (pre-pop), RIP=blob+0x2AC3 (the retf).
  RAX=R8=R10=blob+0x1128 — explained: MP_HEX2/MP_DUMP macros clobber r10 by
  design (thunk64.asm line ~40), so the pre-retf `mov rax,r10` picked garbage —
  COSMETIC, frame is what matters and it's right. Note MP_* clobber r10/rax!
- The dump immediately before check_exception in -d int logs is the OTHER
  (32-bit firmware) core's state — don't mistake it for the faulting core.

## THE RETF→FAR-JMP DECISION (RESOLVES THE OLD e=0000 PUZZLE)
- efi32boot's PROVEN transition.asm (32-bit BG → boots, works) uses for
  long→compat: **`jmp far [rsp]` m16:64 (FF 2C 24)** with PG still ON, frame =
  8-byte offset @ [rsp], 2-byte sel16 @ [rsp+8] — byte-identical layout to our
  verified frame. Their empirical comment: "a retf … faults instead".
- SWAPPED in thunk64.asm: old `mov rax,r10 / retf` → `mov rax,r10 / jmp far
  [rsp]` (FF 2C 24 verified in blob at offset 0xAC4..0xAC6; in_compat moved to
  blob+0xAC7). NO frame changes needed; rsp stays 0x7EAE7EA8 (far-jmp does not
  pop).
- RESULT (m2_t25): **#GP e=0000 count = 0** — down-leg healed; C probe printed.
  Old e=0000 puzzle was retf-in-OVMF32-context-specific; far-jmp bypasses
  helper_ret_protected entirely.

## NEW FAULT (ACTIVE DEBUG): #GP e=0010 at blob+0xBD5, CS=0x18 compat
- Evidence m2_t25: `166: v=0d e=0010 IP=0018:0x7EAD2BD5` (payload=0x7EAD2000 →
  blob+0xBD5), RSP=0x7EAE7EA0 (= 0x7EAE7EA8-8 ⇒ `sub esp,8` EXECUTED), RAX=0x10,
  then #DFcascade (`v=08`). Every crash identical (~17 boots verified for,
  counts may differ now — re-count on latest image).
- in_compat decoded from bytes (objdump -M i386, blob+0xAC7 onwards):
  call/.self (pop ebp/sub ebp,5) → sgdt [ebp+0x651] → 'C' print → CP_DUMP ×2
  nibble loops → '||' → `lgdt [ebp+0x7F9]` (saved_gdtr32 6-byte form) /
  `lidt [ebp+0x803]` → sgdt probe → 'F' print → `mov eax,cr0; and
  eax,0x7fffffff; mov cr0,eax` (PG cleared ⊂ blob+0x…BCC-0xBCE) → `sub esp,8`
  → `lea eax,[ebp+0x10F]` (up-path target, blob+0xBD5 = FAULT POINT) →
  `mov [esp],eax` / `mov eax,[ebp+0x7BD]` (saved_cs32=0x10) → `mov [esp+4],eax`
  → `cb` 4+4 retf to in_32bit (≈blob+0xBD6..0xBD9).
- No 'F' probe in serial output (or printed garbled — only ~20 backticks 0x60
  after the U-dump, then reboot): death between C-probe print and F-probe.
  Serial garble itself unexplained (C + hex digits expected; got 0x60s) — may
  just be the compat CP_DUMP hexchars indexing misbehaving (r10/rax clobber
  analog in CP_ macros) — LOW priority, fault is the thing.
- RAX=0x10 at fault + e=0010 ⇒ possible meanings: QEMU-specific GP raise with
  fixed error code 0x10 somewhere in CR0-write validation (helper_write_cr0 —
  misc_helper.c NOT yet fetched!), or segment-load GP with selector 0x10.
- Working theories: (a) helper_write_cr0 vetoes PG-clear under some compat/
  LME condition with e=0x10; (b) LEA-decode/rollback subtlety surfacing the
  NEXT instruction's PC instead of the faulting one; (c) LGDT/LIDT (saved
  firmware blobs) sideffect. NOTE: LGDT/LIDT execute BEFORE the F print → if
  F never printed, LGDT/LIDT may be the actual fatal step (with PC only
  advancing later). CHECK the F-probe absence vs lgdt placement FIRST.

## ACTIVE ISOLATION TEST: farret2.img (payload-state MIMIC) — PASSING (SOLVED)
- File: /tmp/opencode/farret2_test.asm + farret2.img (clean, 512 B, 23:42).
- Mimics: CR4=0x660, EFER|=0x500, CR0 base|PE|PG single write, FULL 4-GiB map
  (PDPT@0x11000 + 4 PDs @0x12000–0x15FFF, 2 MiB pages, P|RW|PS=0x83), HIGH GDT
  @0x7EAD3000 (0x18 compat entry + gdtr blob @0x7EAD3080 = u16 limit + u64
  base, `(0x7EAD3000<<16)|0x1F`), HIGH stack rsp=0x7EAE7EA8, snippet copied to
  0x7EAD2000, far-ret (lretq 8+8, sel 0x18) → C printed in compat.
- **MUST run with `-m 2048`** — under -m 128 the high addresses are unassigned
  physical → nonsense #UD at RIP=3 (the earlier "failure" was purely that).
- Early hard rules for the mimic: snippet must be `bits 32` (bits-16 encodes
  wrong in D=1 compat → silent hlt, no C, no exception); `cld` before rep
  movsb (DF dirty); NASM 512-byte TIMES budget tight; decode 64-bit regions
  with `objdump -D -b binary -m i386:x86-64`, 16/32-bit with `-m i386`; od
  byte grouping misleads.
- Verdict drawn: payload's old retf GP(0) was NOT architectural state, NOT
  -smp count, NOT page-table shape, NOT high-GDT/stack/code. It was retf-
  specific in the OVMF32 firmware context (or something else that far-jmp
  sidesteps). farret2 remains USEFUL as a fast-cycle testbed for the NEXT
  unknowns (bisect CR0 NE|MP=0x32, firmware-style IDT lidt, etc.).

## DIAGNOSIS HISTORY (older summary, kept short)
- B2 compat dance retained: g[3]=0x00CF9A000000FFFF sel 0x18, bridge limit 0x1F,
  8+8 retfq down / in_compat: lgdt+lidt firmware 6-byte forms (first 6 bytes of
  the 10-byte saved_* blobs), clear PG, 4+4 retf to in_32bit. B1 (`db 0x66`
  retf) dead empirically.
- Bug #5 (.refptr) fix predates this session; data-side ops (saved_gdtr32/
  saved_idtr32 fill + prints) VERIFIED = do not re-audit.
- Trampoline capture order fixed (capture right after cli, BEFORE bridge lgdt).
- Staging layout: pml4@+0 pdpt@+0x1000 pds@+0x2000 gdt64@+0x6000 params@+0x7000
  tramp@+0x8000 payload@+0x9000 stack_top@+0x1F000.
- BOOT_PARAMS capture: +24 gdtr blob6 · +32 idtr blob6 · +40 cr0 +44 cr3
  +48 cr4 · +52 cs..+72 gs (dwords) · +76 esp. C struct does NOT model it.

## WHAT REMOVED/WARNINGS
- MP_HEX2/MP_DUMP/MP_DUMP2 clobber rax, r10 (and r11 holds the char). Anything
  saved in rax/r10 pre-MP_DUMP is LOST — current U-probe's `mov rax,r10` before
  retf picks garbage (harmless but confusing in fault dumps). MP_/CP_ probe
  block + payload64.c debug prints are MARKED DELETABLE after the round trip
  works.
- farret2 512-byte budget is tight: repeated `(times 510-($-$$))` errors mean
  trim code (dropped bail block, dup stores) — keep letters minimal.

## IMMEDIATE NEXT STEPS (in order, v4 — UPDATED after byte-decode)
1. Count backticks in m2_t25.log serial tail → deduce how many CP probes
   actually fired (C + digits + '|'≈18 per probe under the LSR-clobber
   theory). Then confirm CP_PUT clobber bug by reading thunk64.asm CP_PUT vs
   MP_PUT macro shapes.
2. Verify saved_gdtr32 runtime content: grep m2_t25.log for
   "saved_gdtr32      : " (expect `47 00 00 f0 c3 7f 00 00 00 00`).
3. Bisect the gap: is lgdt [ebp+0x7F9] reached/executed? Insert ONE extra
   probe letter between lgdt and lidt (and one after lidt, before sgdt probe
   / 'F') in thunk64.asm — rebuild + boot (esp_m11.img / m2_t26.log).
4. If lgdt executed with correct content but retf still GP(0x10): suspect
   QEMU keeps CS-cached GRANULAR/DB bits... then mimic efi32boot exactly:
   clear EFER.LME (rdmsr 0xC0000080, and eax,0xFFFFFEFF, wrmsr) and/or clear
   CR4.PAE around the PG-clear inside in_compat, per transition.asm.
5. After round-trip works: delete MP_/CP_ probe block + payload64.c debug
   lines (marked deletable), then M2 = AllocatePool via do_thunk_call.
6. Then the real Windows-image phase (win10ltsc.iso via /mnt/hdd2/Images).
7. Check ~/.zcode/cli/rollout/*.jsonl for concurrent ZCode edits; ANSWER the
   pending user question ("did u use the code from the zcode session?" — NO
   session data was read directly; verify + tell the user plainly).

## BUILD + TEST COMMANDS (copy-paste, CURRENT — m10 active)
```
export PATH="/home/abokaborda/Documents/Default Project/efi32boot/toolchain/usr/bin:$PATH"
cd "/home/abokaborda/Documents/Default Project/efi64boot" && make clean && make all
cp -f BOOTIA32.EFI /home/abokaborda/w64work/ && cd /home/abokaborda/w64work
rm -f esp_m10.img && dd if=/dev/zero of=esp_m10.img bs=1M count=260 status=none
mformat -i esp_m10.img && mmd -i esp_m10.img ::/EFI ::/EFI/BOOT && \
mcopy -i esp_m10.img BOOTIA32.EFI ::/EFI/BOOT/
python3 -c "img=open('esp_m10.img','rb').read(); efi=open('BOOTIA32.EFI','rb').read(); \
print(img.count(efi[:48]), img.count(b'raw cap'))"   # expect: 1 1
timeout 45 qemu-system-x86_64 -accel tcg -cpu qemu64 -m 2048 -smp 2 \
  -display none -serial file:m2_t25.log -bios OVMF32.fd -drive format=raw,file=esp_m10.img \
  -d int -D m2_int25.log
```
- Filter fault ints: `grep -n "^ *[0-9]*: v=0d\|v=08\|v=06\|v=0e" m2_int25.log`
  (timer INT=0x20 lines are routine firmware ticks — ignore).
- Bootsector tests: `nasm -f bin -o X.img X.asm` (toolchain nasm) then
  `qemu-system-x86_64 -accel tcg -cpu qemu64 -m 2048 -display none -serial
  file:X_s.log -drive format=raw,file=X.img [-d int -D X_int.log]`.
  **-m 2048 REQUIRED for farret2-style high addresses** (-m 128 = false #UD).

## KEY FILES
- `thunk64.asm`: battleground; MP_/CP_ probes (marked deletable), B2 down path
  now **far-jmp variant** (`jmp far [rsp]` replacing retf), in_compat,
  in_32bit up path.
- `payload64_entry.asm`: entry stub + FIXED bounded reloc walk (extern reloc_end).
- `payload64.ld`: exports reloc_start AND reloc_end = . (after .reloc).
- `main.c` / `trampoline.asm` / `payload64.c`: unchanged this session.
- Reference: `/home/abokaborda/Documents/Default Project/efi32boot/transition.asm`
  (PROVEN down-switch: jmp far [rsp] long→compat PG-on; then NW/CD clear,
  PG clear keep PE, CR4.PSE clear, EFER.LME clear, 4+4 retf; minimal GDT
  template null/0x08 code32 00CF9A000000FFFF/0x10 data 00CF92000000FFFF;
  lgdt BEFORE the far-jmp).
- Test artifacts: /home/abokaborda/w64work/{esp_m10.img, OVMF32.fd,
  BOOTIA32.EFI, m2_t25.log, m2_int25.log}; newer evidence. Older m2_t22/int22
  = pre-farjmp (retf GP(0)); m2_t25/int25 = current (farjmp works, e=0x10).
- Isolation tests: /tmp/opencode/farret_test.asm+farret.img (PASSING round
  trip), farret2_test.asm+farret2.img (mimic, PASSING — run with -m 2048!),
  farret2_s.log/farret2_int.log; QEMU v8.2 sources in /tmp/opencode:
  seg_helper.c, translate.c, exc_helper.c, cpu-exec-common.c (misc_helper.c
  still to fetch).

## SEE ALSO — critical rules (EFFECTIVE 2026-10-25)
1. FRESH FAT image per run (stale copies hide — proven!!) + `make clean`.
2. Nothing 16-bit-decode-unsafe between `mov cr0` (PG clear) and the far
   transfer; bridge CS for legacy code has D=0 (BUG B class).
3. 32-bit retf frame = EIP@[esp], CS@[esp+4] (4+4); 64-bit retfq = 8+8.
4. Nothing may touch RAX between the CR0 mask and `mov cr0` (BUG A class).
5. No RIP-relative operands in code that leaves long mode (32-bit decodes
   disp32 absolute); upstream target must be ebp-relative (BUG C class).
6. Info-source hygiene: the CORE dump right before `check_exception` in
   -d int logs is the OTHER core; MPSAVE — MP_* probes clobber rax/r10.
