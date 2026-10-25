# Multi-architecture roadmap: 32/64-bit cross-launch, both directions

Goal: for each architecture, launch "the other width" of EFI executables:
64-bit firmware → 32-bit executables, AND 32-bit firmware → 64-bit
executables. x86 is done (efi32boot one way; efi64boot M1 the other).
This document is the honest feasibility map for the rest.

## Verdict table

| Arch | Down (64 fw → 32 exe) | Up (32 fw → 64 exe) | Mechanism |
|------|----------------------|---------------------|-----------|
| x86 / x64 | ✅ **done** (efi32boot) | ✅ **M1 done** (efi64boot) | CR0/CR4/EFER + far jump; both directions are just instructions |
| PPC64 ↔ PPC32 | 🟡 easy mechanically | 🟡 easy mechanically | one `mtmsr` flips MSR[SF]/CM; same MMU, same page tables |
| AArch64 → AArch32 | 🟡 feasible | — | `ERET` with SPSR.nRW=0 at same EL; new page tables needed (AArch32 TTBRx/LPAE formats) |
| AArch32 → AArch64 | — | 🔴 blocked at same EL | nRW can only flip across exception levels; needs EL2/EL3 cooperation |
| RV64 ↔ RV32 | 🔴 impossible directly | 🔴 impossible directly | RV64 cannot execute RV32 encodings; no runtime width switch exists |

Legend: ✅ proven · 🟡 buildable · 🔴 architecturally blocked (needs another
mechanism)

## Per-architecture notes

### PPC64 ↔ PPC32 — next target, both directions
- The 64→32 switch is literally clearing MSR[SF] (server) / MSR[CM] (book-E);
  the reverse is setting it. No MMU format change, no mode-entry dance.
- Work: ABI marshalling (ELFv2 64-bit vs PPC32), TOC handling, GPR
  narrowing, and a test firmware.
- Catch: there is effectively **no 32-bit PPC UEFI firmware in the wild**,
  so this would be a QEMU-hosted research configuration (OpenBIOS/SLOF as
  the "firmware") rather than a real-world launcher. Mechanically HIGH
  feasibility, real-world demand near zero — do it as the cleanest
  demonstration of the concept.

### AArch64 → AArch32 — feasible
- Same mechanism 64-bit Linux kernels use to enter 32-bit userspace:
  exception return into AArch32 at the same EL.
- Work: build AArch32 page tables (short-descriptor or LPAE — different
  descriptor format than AArch64), switch via `ERET`, marshal the calling
  state.
- Firmware side: 32-bit ARM UEFI *does* exist in the wild (AArch32 EDK2
  builds for older boards), so the down direction has a real audience.

### AArch32 → AArch64 — blocked without EL2/EL3
- PSTATE.nRW (width) cannot be flipped at the same exception level; there
  is no instruction that "rises" AArch32 EL1 into AArch64 EL1.
- Options: (a) bounce through EL2 with an HVC if a hypervisor exists (QEMU
  `-M virt` gives us EL2 — easy there, rare on the real 32-bit boards this
  targets); (b) on real hardware this is effectively "rewrite the firmware",
  which defeats the premise.
- Honest verdict: **QEMU-demonstrable, real-hardware impossible** without
  controlling EL2/EL3.

### RISC-V — emulation only
- RV64 cores do not decode RV32 and there is no width-switch mode. The
  cross-launch would have to be MultiArchUefiPkg-style in-firmware CPU
  emulation (as already listed in efi32boot's related-work), not a
  trampoline.
- RV32 EDK2 support is also minimal, shrinking the "reverse" direction to
  a research exercise.

## Phasing

1. **PPC pair** (cleanest concept demo, both directions, QEMU-hosted).
2. **AArch64 → AArch32** (real-world relevant, QEMU `-M virt` + AArch32 EDK2).
3. **AArch32 → AArch64** (QEMU EL2 bounce; document the hardware caveat).
4. **RISC-V** (emulation-based; defer until the rest is done).

Each phase lands in its own directory/repo with the same M1 pattern:
capability check → staging via firmware services → mode-specific page
tables → trampoline → flat payload on serial. The x86 experience (flat
position-independent payloads, dword/qword marshalling, `qemu -d int`
debugging) transfers directly.

## CI note

Per the author's request, CI runs are not triggered from this machine;
the existing workflows (x86 build + TCG boot tests + cross-compile
toolchain checks for aarch64/riscv64/ppc64le) run on push from the web UI.
