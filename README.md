# efi64boot

**Launch 64-bit EFI executables from 32-bit UEFI firmware, when the CPU is
64-bit capable.** The mirror image of
[efi32boot](https://github.com/devtinoimao/efi32boot): there, 64-bit firmware
hosted a complete 32-bit firmware; here, the 32-bit firmware stays in charge
and we add a long-mode trampoline on top.

Status: **M1 milestone proven** (2026-10-25) — `BOOTIA32.EFI` runs on IA32
OVMF under QEMU, detects long-mode CPU support via CPUID, builds a 4 GiB
identity map, switches 32-bit protected mode → long mode, and executes a
64-bit payload (`IA32_EFER.LMA=1` verified on serial).

This is AI-assisted experimental firmware. Not production-safe. Do not use
on real hardware. See `docs/SAFETY.md` in efi32boot for the general hazard
picture.

## How it works (M1)

1. `BOOTIA32.EFI` is a normal IA32 UEFI application loaded by the 32-bit
   firmware.
2. CPUID `0x80000001.EDX[29]` (LM) — refuse to continue on a 32-bit-only CPU.
3. Use the firmware's own 32-bit boot services (`AllocatePages`) to reserve a
   staging region below 4 GiB: page tables, 64-bit GDT, parameter block, the
   embedded 64-bit payload, and a fresh 64-bit stack.
4. Copy in the flat 64-bit payload (position-independent, RIP-relative) and
   the flat 32-bit trampoline (fully position-independent: self-locating far
   jump via call/pop).
5. Trampoline: `cli`, load 64-bit GDT, CR4.PAE, CR3, EFER.LME, CR0.PG, far
   return into the 64-bit code segment.
6. 64-bit payload runs; M1 talks to COM1 directly (no services on the 64-bit
   side yet).

## Bugs found while proving M1 (documented for the next person)

- `EFI_BOOT_SERVICES` starts with a 24-byte `EFI_TABLE_HEADER`; calling
  `gBS+8` calls the header CRC field (`#UD` at garbage EIP).
- The trampoline must fetch its cdecl argument from the stack; reading a
  leftover register gave `CR3` the firmware's `RaiseTPL` pointer → instant
  page-fault storm once paging re-enabled (found via `qemu -d int`).
- 32-bit parameter fields must be read as dwords in 64-bit code; a qword
  `mov rax,[rsi+4]` captures the next field into the upper half and calls an
  address above 4 GiB.

## Build

```bash
make TOOLCHAIN_PREFIX=/path/to/mingw-w64/bin/ all
# needs: x86_64-w64-mingw32-gcc, i686-w64-mingw32-gcc, nasm
```

## Test (QEMU, no real hardware involved)

```bash
# IA32 OVMF firmware (32-bit) + 64-bit CPU = the exact target configuration
dd if=/dev/zero of=esp64boot.img bs=1M count=64
mkfs.vfat -F 32 esp64boot.img
mmd -i esp64boot.img ::/EFI ::/EFI/BOOT
mcopy -i esp64boot.img BOOTIA32.EFI ::/EFI/BOOT/BOOTIA32.EFI
qemu-system-x86_64 -accel tcg -cpu qemu64 -m 2048 -display none \
  -serial file:boot.log -bios OVMF32.fd -drive format=raw,file=esp64boot.img
# grep boot.log for "HELLO FROM 64-BIT LONG MODE"
```

CI runs exactly this on GitHub Actions (TCG; no KVM on runners).

## Roadmap

- **M2**: 64→32 service thunks (mode-switch down per call, MS x64 → IA32
  cdecl marshalling) for a minimal boot-services subset.
- **M3**: manual PE32+ loader + fake 64-bit system table; boot a real
  `BOOTX64.EFI` (GRUB x64 / Windows bootmgr).
- Ports of the *concept* (32-bit firmware on wider CPU) map naturally to
  AArch32→AArch64 and RV32→RV64; the x86 trampoline does not. Cross-compile
  checks for those targets run in CI to keep the tree portable.

## Related

- [efi32boot](https://github.com/devtinoimao/efi32boot) — the reverse
  direction (64-bit firmware hosting 32-bit OVMF), same author, same
  experimental stance.
- EDK2 DUET, MultiArchUefiPkg — prior art in cross-arch UEFI launching.
