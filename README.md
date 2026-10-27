# efi64boot

**Launch 64-bit UEFI firmware from a 32-bit BOOTIA32.EFI boot application, on
64-bit-capable CPU from 32-bit firmware.** The mirror image of
[efi32boot](https://github.com/devtinoimao/efi32boot): there, 64-bit firmware
hosted a complete 32-bit firmware; here, the 32-bit firmware stays in charge
and we add a long-mode trampoline on top.

Status: **M3 milestone achieved — Windows Setup boots** (2026-10-27).
`BOOTIA32.EFI` (IA32) runs on 32-bit OVMF, hands off to a hosted 64-bit
OVMF64 SEC entry, runs the full 64-bit PEI/DXE chain, and boots
`win10ltc.iso` into Windows Setup under QEMU (verified on both TCG and KVM,
`qemu64` and `core2duo` CPUs).

This is AI-assisted experimental firmware. Not production-safe. Do not use
on real hardware.

## What it does

1. `BOOTIA32.EFI` is a normal IA32 UEFI application loaded by the 32-bit
   firmware.
2. CPUID `0x80000001.EDX[29]` (LM) — refuse to continue on a 32-bit-only CPU.
3. `AllocatePages` reserves a staging region below 4 GiB: page tables, a bridge
   GDT, a fresh 64-bit stack, the parameter block, and the embedded 64-bit
   payload (`ovmf64_start`).
4. `relocate()` applies the SEC module's base relocations so it runs at its
   new address.
5. The full 16 MiB firmware MEMFD region (`0x800000`, `0xF80000` bytes) is
   reserved at its build address (matching `OvmfPkgX64.fdf` `[FD.MEMFD]`)
   and zero-initialized, so OVMF64 takes the native (non-TDX) SEC path and
   no PCD patching is needed.
6. The 32→64 trampoline: `cli`, load 64-bit GDT, CR4.PAE, CR3, EFER.LME,
   CR0.PG; far-return into the 64-bit code segment; build a 256-entry IDT;
   emit the `p`/`T`/`A` serial probes; perform pre-handoff CPU hygiene (clear
   CR0 NW/CD, CR4.PSE, set `IA32_MTRR_DEF_TYPE` to E=1/FE=0/type=WB per
   OVMF64's asserts).
7. Jump to OVMF64's SEC entry (`rcx =` BootFV, `rdx =` temp-RAM stack top
   `0x820000`). OVMF64's `SecMtrrSetup` / PlatformPei `MemDetect` asserts
   pass, followed by PEI/DXE/BdsDxe.
8. `delete_self_from_esp()` zeroes the start of `BOOTIA32.EFI` on the ESP so
   BdsDxe can't re-select it; `SetVariable BootOrder` prioritizes the DVD.

Resulting chain (verified):
```
BOOTIA32.EFI (32-bit) -> OVMF64 SEC (64-bit) -> PEI -> DXE -> BdsDxe ->
DVD boot -> Windows Setup
```

This is the first verified run of a real 64-bit firmware on top of a 32-bit
UEFI application, where earlier 64-bit-launch-only attempts (existing
"EFI64Boot" public repo) stopped at payloads.

## Bugs found and fixed along the way

- `EFI_BOOT_SERVICES` starts with a 24-byte `EFI_TABLE_HEADER`; calling
  `gBS+8` calls the header CRC field (`#UD` at garbage EIP). (Carried over
  from efi32boot lineage.)
- The trampoline must fetch its cdecl argument from the stack; reading a
  leftover register gave `CR3` the firmware's `RaiseTPL` pointer → instant
  page-fault storm once paging re-enabled (found via `qemu -d int`).
- 32-bit parameter fields must be read as dwords in 64-bit code; a qword
  `mov rax,[rsi+4]` captures the next field into the upper half and calls
  an address above 4 GiB.
- Forcing `byte[0x80B000] = 2` selected OVMF64's TDX `tdcall` path
  (`mov $0x80b000,%eax; cmpb $0x2,(%eax); jne`) → triple-fault/reset
  *before* any SEC output. Removed. Verified the byte reads `0`.
- MEMFD region was only 32 pages (128 KiB) but OVMF64's SEC stub writes temp
  RAM at `0x814000` and sets `ESP = 0x820000` — both outside the reservation.
  Enlarged to the full `0xF80000` (16 MiB) the OVMF64 build expects.
- OVMF64's MTRR_DEF_TYPE after the outer 32-bit firmware does not satisfy
  PlatformPei `MemDetect` asserts `(MtrrDefType & BIT11)!=0`,
  `(MtrrDefType & BIT10)==0`, `(MtrrDefType & 0xFF)==6`; set it to
  `E=1/FE=0/type=write-back (0x806)` before the handoff.
- The earlier "apparent" OVMF64 shell was actually OVMF32's shell;
  `Image type X64 can't be started on IA32 UEFI system` confirmed this.

## Build

```bash
make TOOLCHAIN_PREFIX=/path/to/mingw-w64/bin/ all
# needs: x86_64-w64-mingw32-gcc, i686-w64-mingw32-gcc, nasm
```

## Test (QEMU, no real hardware involved)

```bash
# IA32 OVMF firmware (32-bit) + 64-bit CPU = the exact target configuration
dd if=/dev/zero of=esp.img bs=1M count=260
mkfs.vfat -F 32 esp.img
mmd -i esp.img ::/EFI ::/EFI/BOOT
mcopy -i esp.img BOOTIA32.EFI ::/EFI/BOOT/BOOTIA32.EFI
qemu-system-x86_64 -accel tcg -cpu qemu64 -m 2048 -display gtk \
  -serial file:boot.log -bios OVMF32.fd \
  -drive format=raw,file=esp.img \
  -drive file=/path/to/win10ltc.iso,format=raw,if=ide,media=cdrom
# Hit a key at "Press any key to boot from CD or DVD..."
```

KVM + `core2duo` (Intel Mac class 32-bit-firmware + 64-bit-CPU setup) also
works; use `-accel kvm -cpu core2duo`.

CI runs the TCG build on GitHub Actions (no KVM on runners).

## Remaining (housekeeping, not blockers)

- ESP self-delete still fails (FAT driver returns `0x80000003`/`0x00000002`);
  currently handled by zeroing the file's first 64 bytes before handoff.
- Debug probes (`p`/`T`/`A`/`B`/`n`/`S`/`R`) still present.
- Serial output during OVMF64 SEC/DXE is mostly `debugcon` port `0x402`
  (this build routes `DEBUG` there, not COM1) — capture with `-debugcon`.

## Related

- [efi32boot](https://github.com/devtinoimao/efi32boot) — the reverse
  direction (64-bit firmware hosting 32-bit OVMF), same author, same
  experimental stance.
- EDK2 DUET, MultiArchUefiPkg — prior art in cross-arch UEFI launching.