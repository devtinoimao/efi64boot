# EFI64Boot development context (Nov 09 2026)

Role
- This repo (target): 32-bit IA32 `BOOTIA32.EFI` hosting a 64-bit OVMF64 firmware,
  hands off to OVMF64 SEC, boots Windows x64 from `win10ltc.iso`.
- Reference repo: ../efi32boot (64-bit host -> 32-bit OVMF32 firmware). The SOLE
  reference; public EFI64Boot repo is NOT a reference even though this repo is
  pushed there.
- Direction: 32-bit host -> 64-bit inner firmware.
- AI-assisted / experimental. Not production-safe. Do not use on real hardware.

Status: M3 milestone achieved (2026-10-27). BOOTIA32.EFI runs on 32-bit OVMF32,
hands off to a hosted 64-bit OVMF64 SEC entry, executes the complete 64-bit
PEI/DXE chain, boots `win10ltc.iso` to Windows Setup under QEMU (TCG and KVM,
qemu64 and core2duo). VMware / real i5-4570 (CSM) not yet validated.

Hardware / VMM support matrix
- QEMU TCG qemu64 (default) - tested
- QEMU KVM qemu64 / core2duo - tested
- VMware 0x8086:0x7190 host bridge - untested; AcpiTimerLibConstructor may
  ASSERT on "Unknown Host Bridge Device ID: 0x7190"
- Real i5-4570 with CSM on - not validated; CSM == legacy BIOS, no UEFI, so
  BOOTIA32.EFI never runs
- Real i5-4570 with CSM off - candidate target (host bridge ID logged)

Build
    make TOOLCHAIN_PREFIX=/path/to/mingw-w64-bin/ all
    # needs i686-w64-mingw32-gcc, x86_64-w64-mingw32-gcc, nasm
    # -> BOOTIA32.EFI (PE32 IA32, ~2 MiB incl. embedded OVMF64)

Test (QEMU)
    dd if=/dev/zero of=esp.img bs=1M count=260
    mkfs.vfat -F 32 esp.img
    mmd -i esp.img ::/EFI ::/EFI/BOOT
    mcopy -i esp.img BOOTIA32.EFI ::/EFI/BOOT/BOOTIA32.EFI
    qemu-system-x86_64 -accel kvm -cpu core2duo -m 2048 -display gtk \
      -serial file:boot.log -bios OVMF32.fd \
      -drive format=raw,file=esp.img \
      -drive file=/path/to/win10ltc.iso,format=raw,if=ide,media=cdrom
    # Hit a key at "Press any key to boot from CD or DVD..."

Checklist of serial probes:
- p = 32-bit mode, paging on
- T = 64-bit long mode entered
- A = post-IDT (immediately before SEC call)
- B = byte at 0x80B000 (TDX flag, must read '0')
- n = MTRR hygiene done (MSR 0x2FF = 0x806)
- S = SEC entry returned (FAIL if printed)
- Z (halt loop) = an exception was caught by the per-vector watchdog;
  preceding `XC##` shows the vector number (13 GP, 14 PF, ...)
- (p/T/A/B/n/S/R/Z); "hello from real OVMF64" appears once BdsDxe boots

Architecture
Outer (host) 32-bit UEFI (booted by -bios OVMF32.fd under QEMU, or a
   32-bit firmware on target hardware)
   runs BOOTIA32.EFI in 32-bit PE/COFF context
   -> sets BootOrder (DVD-first: 3,1,0,2)
   -> AllocatePages EfiLoaderData, copy OVMF64 image, relocate() SEC module
   -> reserve MEMFD 0x800000..0x880000 (16 MiB), zero-initialized
   -> build 4-level identity page tables (0..4 GiB, 2 MiB pages):
        pml4 @ 0x800000, pdpt @ 0x801000, pds @ 0x802000..0x805000
   -> load 64-bit GDT (5 entries) at 0x806000, params at 0x807000
   -> build 256-entry IDT with per-vector watchdog stubs (+0x0B000 offset)
   -> load IDT at 0x809000, IDTR at 0x80A000
   -> stash pml4_phys, stack_top, gdt_phys, firmware_entry, firmware_fv
   -> delete_self_from_esp(): zero 64 bytes of BOOTIA32.EFI so BdsDxe can't
      re-select it (FAT driver rejects both truncate and delete)
      hosts 1 SimpleFileSystem handle at 0x22,0x5b,0x4e,0x96 GUID
   -> ExitBootServices(image, MapKey)
   -> trampoline 32->64 (BITS 32 entry):
        sgdt/sidt state dump, cli, load bridge GDT (limit 0x27), lgdt
        CR4.PAE on, CR3 = pml4, EFER.LME on, CR0.PG on
        `call .next / pop eax / sub .next-start / add lm_entry-start`
        `push CODE64_SEL (0x08); push eax; retf` (far return)
      BITS 64 lm_entry:
        emit 'T', params from rsi, rbp = firmware_fv, rsp = 0x820000 and -16
        build 256 per-vector fault stubs: push imm8; jmp fault_common
        fill 256 IDT entries pointing at stubs, lidt, cli
        emit 'A', probe byte at 0x80B000, force-free hygiene:
          MSR 0x2FF (IA32_MTRR_DEF_TYPE): rdmsr; EAX=0x806 (E=1/FE=0/WB);
          wrmsr; print 'n'
        cli; call firmware_entry (addr + SEC_FV_OFFSET + SEC_ENTRY_RVA)
   -> 64-bit OVMF64 SEC runs its normal (non-TDX) path
      (move $0x80B000; cmpb $0x2, (EAX); jne ...)
      fills 0x814000..0x820000 with 0x5AA55AA5
      SecCoreStartupWithStack(BootFv, TopOfCurrentStack)
      -> SecMtrrSetup, MemDetect (MTRR asserts), PlatformPei, PEI dispatcher,
         DxeIpl, DXE core, BdsDxe -> loads Boot0001 "UEFI QEMU DVD-ROM ..."
         -> ISO bootmgfw.efi (if present)

Progress markers emitted: p / T / A / B(letter) 0(byte) / n - probe `pTAB0n`

Key bugs fixed along the way
- Far jump into 64-bit used a near `jmp` with synthesized EIP -> never
  actually left 32-bit mode; switched to call/.next / push sel/eip / retf.
  Result: EDX issue above = EDX along the path, now works.
- EFI_BOOT_SERVICES table header at +0 through +16 means function pointer
  table starts at +24; calling gBS + 8 called CRC field.
- Trampoline must read its cdecl arg from the stack — leftover register gave
  CR3 == RaiseTPL pointer -> page faults once paging on.
- Stopping at 32-bit dwords reading 64-bit memory corruption.
- Forcing `byte[0x80B000] = 2` picked TDX `tdcall` path (triple-fault before
  any SEC output). Removed, zeroed MEMFD.
- MEMFD region was 32 pages (128 KiB) but SEC writes to 0x814000 and
  ESP=0x820000. Enlarged to 16 MiB (OvmfPkgX64.fdf [FD.MEMFD] size 0xF80000).
- MTRR_DEF_TYPE after the outer 32-bit firmware doesn't satisfy OVMF64's
  MemDetect asserts (E=1, FE=0, default WB=6) — set to 0x806 pre-hand-off.
- delete_self_from_esp write0 status 0x80000003 / delete status 0x00000002
  → zero 64 bytes as safety net only.
- "apparent 64-bit shell" was OVMF32's shell (Image type X64 can't be started
  on IA32 UEFI system) — corrected.
- GDB evidence: CPU in long mode, RIP=0x8472c9 in OVMF64 PEI code before
  the MTRR assert it hung on.

Key firmware values
- OVMF64_SEC_FV_OFFSET 0x1BC000 (offset inside the 1.875 MiB image)
  → firmware_fv = firmware_base + 0x1BC000
  → firmware_entry = firmware_base + 0x1BC094 (SEC_FILE_OFFSET) + 0x9DF0 (RVA)
- OVMF64_TEMP_RAM_STACK = 0x820000 (must be reported to SEC as stack top)
- SEC entry file offset 0x1C5E84 (BITS 64) — disassembled by us
  (ref: OvmfPkg/Sec/X64/SecEntry.nasm)

Host bridge PCI ID dump
- efi64boot prints "efi64boot: Host bridge PCI ID = 0xXXXXXXXX" early
- QEMU: 0x12378086 (0x8086:0x1237, QEMU i440FX/PIIX4)
- VMware: 0x71908086 (0x8086:0x7190, Intel 440BX) — temp-override to 0x1237
- real hardware: variable; reported via log

Supported environments
- QEMU TCG qx6 (default, 64-bit host) -- tested
- QEMU KVM -- tested
- VMware / real i5-4570 (CSM off) -- NOT validated. inner OVMF64 makes
  QEMU assumptions (mem detect, host bridge PCI ID, Debug Port I/O). CSM-on
  means no UEFI at all -> BOOTIA32.EFI never invokes.
- documented in README.md

Repository / releases
- main: pushed up to latest commit (context.md + host-bridge logging).
- tags: v1.0, v0.2.1
- remote: https://github.com/devtinoimao/efi64boot.git
- embedded: OVMF64.fd (system OVMF_CODE.fd), OVMF32.fd (outer -bios firmware)

Known limitations
- delete_self_from_esp(): FAT reports write/truncate unsupported (0x80000003 /
  0x00000002); we zero 64 leading bytes; not robust
- serial probes (p/T/A/B/n/S/R/Z) still present; remove after final validation
- debug output routes to debugcon (port 0x402), NOT COM1 — capture via
  `-debugcon file:...`
- gen_ovmf64.py hard-codes TEMP_RAM_STACK (PCDs not scanned)

Open items
1. Remove probes, ship cleanup release (v0.2.2 or v1.0 followup)
2. Capture console on hardware; CSM off + UEFI-only verified
3. Raise efi64boot as a separate release entry-point for clean boots under
  full Screen console (no `Press any key`)
4. Update forum drafts / push final README

Reference map (EFI32Boot, DO NOT copy code; adapt)
- main.c: PrepareHostedFirmware, RelocateSecModule, PatchSecTempRam,
  FindFreeRegion, AllocateBelow4G, DeleteEspFile, ExitBootServices, Mark
- transition.asm: 64->32 (GDT handover page, CR0 NW/CD, CR4.PSE,
  MTRR 0x2FF bit10, EFER.LME off)
- tools/gen_ovmf32.py: PCD immediate scanning (temp base/size/stack top)
- build/ovmf32_constants.h: OVMF32_MEMFD_BASE 0x40000000,
  OVMF32_MEMFD_SIZE 0x01310000, OVMF32_TEMP_RAM_BASE 0x40010000,
  OVMF32_TEMP_RAM_SIZE 0x00010000, OVMF32_STACK_TOP 0x40020000,
  OVMF32_INIT_STACK_VALUE 0x5AA55AA5

Contributors / Credits
- efi64boot is AI-assisted experimental firmware; not production-safe

## 2026-11-10 -- above-4G decoding, Q35, VMware, early Intel Mac detection

> **SUPERSEDED 2026-11-11** by the universal portability layer below.
> Kept as history: this section describes the earlier VMware-specific fix.

- main.c now includes ../shared/platform_quirks.h. The two broken host-bridge
  blocks (byte-swapped 0x80867190 compare, write to port 0x0CFA, read-only
  DID spoof) are gone. One correct detection logs VID:DID and the probed
  ACPI PM device; pq_enable_pm_io() only acts if PM I/O is not yet enabled.
- Above-4G decoding (the "above 8GB" item, corrected to RAM above 4 GiB):
  efi_memory_top() scans the UEFI memory map (stride = DescSize),
  gCmosHighMemBytes reads CMOS 0x5B..0x5D and pq_cmos_set_highmem()
  publishes it back for OVMF64's fallback path. extend_page_tables_high()
  fills pdpt0[4..] and PML4[1] with identity mappings using 1 GiB pages
  (CPUID 80000001:EDX[26]) or a bounded 64 GiB 2 MiB fallback for CPUs
  without PDPE1GB (e.g. Core 2 Duo). Physical address width comes from
  CPUID 80000008. Page-table pool is allocated below 4 GiB.
- Intel 2005/2006 Macs (915G/945GM + ICH7-M, 32-bit EFI): classified as
  PQ_PLAT_INTEL_MOBILE, ICH7 LPC (00:1F.0) probed, QEMU-only CMOS writes
  suppressed, and a clear diagnostic on Core Duo/Yonah (no long mode).
  This is detection + graceful diagnostics, NOT a validated Mac boot:
  OVMF expects fw_cfg/PAM/QEMU CMOS that real Apple hardware does not have.
- patches/ovmf-platform-portability.patch (from shared/) applies to edk2-stable
  and must be rebuilt into OVMF64.fd. OVMF64.fd was rebuilt 2026-11-10
  (sha256 9e24f009...); new ovmf64_constants.h: SEC_FV 0x1AC000, entry RVA
  0xA400, link base 0xFFFCC094, reloc 0xEFC0/0xC0. Flash base 0xFFE20000,
  image size 0x1E0000 and temp RAM stack 0x820000 unchanged.
- Reproduce: `bash ../shared/rebuild_ovmf.sh` (X64 build temporarily restores
  MEMFD_BASE_ADDRESS = 0x800000 in the shared FDF include).

Validated under QEMU 10.2.2 TCG with the rebuilt OVMF64: -M pc -m 2048,
-M q35 -m 2048 (8086:29C0 / ICH9 PM 00:1F.0), -m 12288 -cpu qemu64
(PDPE1GB=0 -> 2 MiB fallback), -m 12288 -cpu max (PDPE1GB=1 -> 1 GiB
pages). All reach inner BDS / EFI Internal Shell with no XC## watchdog.
12 GiB run: RAM top 0x340000000, CMOS-high 0x240000000, 40 phys bits,
identity map extended to 0x1340000000.

Still open: gen_ovmf64.py hardcodes the temp RAM stack at 0x820000; ESP
self-delete is still unreliable; real VMware and real Mac hardware untested.

## 2026-11-11 -- universal, no per-machine build

The 2026-11-10 chipset-specific fix is superseded. One `OVMF64.fd` and one
`BOOTIA32.EFI` now work on any host bridge.

- Firmware (`../shared/patch_ovmf_platform.py`): DIDs with dedicated OVMF
  code pass through; every other DID takes a generic fallback. ACPI PM
  discovered by PCI class code (06:80 / 06:01) from any vendor; PM register
  block and ACPI-enable bit resolved at runtime. Patch:
  `../shared/patches/ovmf-platform-portability.patch` (copy in `patches/`).
- Loader (`../shared/platform_quirks.h`): DID is a log label only. PM device
  reported as `class %04x style %u`. CMOS high-mem publishing no longer uses
  a platform allowlist -- `pq_cmos_set_highmem()` does a
  write/readback/restore scratch probe first and skips cleanly if those bytes
  are not plain RAM.
- `-DPQ_TEST_FORCE_UNKNOWN_DID` forces the loader to see `8086:DEAD` for a
  repeatable universality test on ordinary QEMU.
- `OVMF64.fd` rebuilt (sha256 c99ccfca...), SEC entry RVA 0xA610, temp RAM
  stack 0x820000 unchanged; `BOOTIA32.EFI` relinked.
- Reproduce everything: `bash ../shared/rebuild_ovmf.sh`.

Validated QEMU 10.2.2 TCG: pc/2G, q35/2G (class 0601 PM at 00:1F.0),
pc/12G with PDPE1GB=0 and =1, plus a forced-unknown-DID run that still found
`00:01.3 class 0680` and booted to inner BDS. No XC## watchdog, no ASSERT.
Real VMware / real Mac hardware still untested.

## 2026-11-11 (evening) -- OS-neutral, VMware validated, packaged

- Scope correction: only the main C/asm WinPE code was removed. All test
  artifacts, ESP images and the demo GIF/MP4 were restored from Trash.
- This loader no longer names, loads or deletes any OS-specific file; it
  always hosts the inner firmware, whose BDS boots any EFI OS. efi64boot
  also dropped the DVD-first BootOrder override.
- REAL VMware Workstation 26.0.1 pass (serial log:
  efi32boot/vmware_universal_test.log): host bridge 8086:7190, ACPI PM at
  00:07.3 class 0680, generic fallback, ABCDEFGH -> SEC/PEI/DXE/BDS,
  zero ASSERTs and no Unknown Host Bridge. Original 0x7190 assert is gone.
- EDK2 fork https://github.com/devtinoimao/edk2 branch `hosted-firmware`
  (599a612f19) is now the `edk2` submodule in both repos. shared/ is
  vendored into each repo; Makefiles use -Ishared -I../shared.
- README/guides humanised, MIT LICENSE + NOTICE + SPDX tags added, all
  /tmp/opencode paths removed, ISO-based QEMU test commands documented
  (user supplies the ISO), proper compile instructions added.
- QEMU pc/q35 (and 12 GiB for efi64boot) re-validated after the cleanup.
