# efi64boot

**A 32-bit firmware, a 64-bit CPU, and a 64-bit operating system that should
not be able to boot. Now it can.**

Early Intel machines — the 2005/2006 transition kits and the first Intel
MacBooks — shipped 32-bit EFI on processors that were perfectly capable of
64-bit mode. Their firmware cannot load a 64-bit EFI application, so a normal
64-bit OS installer simply refuses to start.

`efi64boot` is the mirror image of
[efi32boot](https://github.com/devtinoimao/efi32boot). There, a 64-bit host
firmware carries a 32-bit inner firmware. Here a 32-bit `BOOTIA32.EFI`
carries a complete 64-bit OVMF image, switches the CPU into long mode, and
lets that firmware boot the OS.

> AI-assisted experimental firmware. Not production-safe. Read the safety
> notes before trying it on real hardware.

## How it works

```
32-bit host firmware
    └── BOOTIA32.EFI  (this project)
            ├── CPUID check: no long mode -> clean refusal
            ├── copy OVMF64 below 4 GiB, rebase its SEC core
            ├── reserve the 16 MiB MEMFD region at 0x800000
            ├── identity-map RAM above 4 GiB (1 GiB pages, or a 2 MiB
            │   fallback for CPUs without PDPE1GB)
            ├── long mode: CR4.PAE -> CR3 -> EFER.LME -> CR0.PG -> far return
            └── OVMF64 SEC -> PEI -> DXE -> BDS -> any 64-bit EFI OS
```

Two things are worth calling out.

**It is OS-neutral.** The loader does not name, delete or prioritise any OS
file, and it no longer rewrites `BootOrder`. The inner firmware's BDS decides
what to boot, so Linux, Windows, BSD or anything else with a 64-bit EFI
loader is equally welcome. It does zero its own `BOOTIA32.EFI` on the ESP so
BDS cannot re-enter the loader — that is about our own file, not yours.

**It handles large memory.** The original page tables only mapped 0–4 GiB, so
any machine with RAM above 4 GiB faulted during SEC. The loader now reads the
UEFI memory map and CPUID physical-address width, publishes the high-memory
size through CMOS `0x5B..0x5D` (only after a write/readback/restore scratch
probe confirms those bytes are safe to touch), and extends the identity map
past 4 GiB.

Platform detection is generic: host-bridge IDs are log labels only, and the
ACPI PM device is discovered by PCI class code from any vendor. One firmware
build works on QEMU i440FX, Q35, VMware and unknown chipsets.

## What's in here

| file | what it does |
| --- | --- |
| `main.c` | UEFI bootstrap, firmware relocation, above-4G page tables, hand-off |
| `trampoline.asm` | 32→64 long-mode switch, per-vector fault watchdog IDT |
| `blobs.S` | embeds `OVMF64.fd` and the trampoline/payload binaries |
| `tools/gen_ovmf64.py` | extracts SEC offsets from a firmware image |
| `ovmf64_constants.h` | generated from the shipped `OVMF64.fd` |
| `shared/` | platform-neutral quirks + EDK2 patch/rebuild tooling |
| `edk2/` | git submodule: the patched [EDK2 fork](https://github.com/devtinoimao/edk2/tree/hosted-firmware) |
| `OVMF32.fd` | a 32-bit outer firmware, used for QEMU testing |

## Status

| environment | result |
| --- | --- |
| QEMU 10.2.2 `-M pc`, 2 GiB | hand-off → SEC → PEI → DXE → BDS, no faults |
| QEMU 10.2.2 `-M q35`, 2 GiB | same; PM found at `00:1F.0` by class `06:01` |
| QEMU `-m 12288` (no 1 GiB pages) | 2 MiB fallback; RAM top `0x340000000`, map to `0x1340000000` |
| QEMU `-m 12288 -cpu max` | 1 GiB page path, same clean hand-off |
| Forced unknown DID (`8086:DEAD`) | passes — no behaviour keyed to a chipset |
| VMware, early Intel Macs | not validated end-to-end for this loader; detection + diagnostics only |

The universal firmware patch itself **was** validated on real VMware
Workstation 26.0.1 through the sibling `efi32boot` loader: `8086:7190`, PM at
`00:07.3`, full chain to BDS, zero asserts. VMware's EFI here is 64-bit, so
it exercises the 64-bit-host path rather than this loader's 32-bit-host path.

## Build it

Needs Python 3, `make`, a MinGW-w64 cross toolchain and NASM:

```bash
git clone --recurse-submodules https://github.com/devtinoimao/efi64boot.git
cd efi64boot

# sibling repo contains the private toolchain helper
export PATH="$PWD/../efi32boot/toolchain/usr/bin:$PATH"
# or point at your own:  make TOOLCHAIN_PREFIX=/path/to/mingw/bin/ all

make all                 # -> BOOTIA32.EFI
```

`OVMF64.fd` is included, so EDK2 is not needed just to compile the loader.

### Rebuilding the embedded firmware

The `edk2` submodule points at branch `hosted-firmware` of
`devtinoimao/edk2`. The X64 build needs `MEMFD_BASE_ADDRESS = 0x800000`
(IA32 uses `0x40000000`), so the rebuild script restores that value for the
X64 pass and puts the shared FDF include back on exit:

```bash
git submodule update --init --depth 1 edk2   # if not cloned recursively
bash shared/rebuild_ovmf.sh
```

Builds run inside `localhost/edk2-builder:bookworm` (GCC 12, NASM, iasl,
EDK2 BaseTools). `shared/patch_ovmf_platform.py` is the idempotent source of
truth; `shared/patches/ovmf-platform-portability.patch` is a plain
`git apply`-able diff for anyone who prefers patches.

## Test it

### Quick smoke test (no OS needed)

```bash
rm -f build/esp.img
dd if=/dev/zero of=build/esp.img bs=1M count=64
mkfs.vfat -F 32 build/esp.img
mmd -i build/esp.img ::/EFI ::/EFI/BOOT
mcopy -i build/esp.img BOOTIA32.EFI ::/EFI/BOOT/BOOTIA32.EFI

qemu-system-x86_64 -accel tcg -cpu qemu64 -m 2048 -smp 2 \
  -bios OVMF32.fd \
  -drive format=raw,file=build/esp.img \
  -serial file:build/boot.log -display none -no-reboot
```

Look for `pTAB0n` in `build/boot.log` (long mode reached, SEC called) and the
`BdsDxe` lines. `XCnn` would mean the watchdog caught an exception.

### Booting a real OS from an ISO you supply

No OS media is shipped here. Bring your own ISO — a WinPE/Windows installer
is one example, a Linux or BSD installer is another:

```bash
ISO=/path/to/your/os.iso             # you provide this file

qemu-system-x86_64 -accel tcg -cpu qemu64 -m 2048 -smp 2 \
  -bios OVMF32.fd \
  -drive format=raw,file=build/esp.img \
  -drive file="$ISO",format=raw,if=ide,media=cdrom \
  -serial file:build/boot.log -display gtk -no-reboot
```

Hit a key at the "Press any key to boot from CD or DVD..." prompt if your ISO
has one. `-accel kvm -cpu core2duo` also works and is closer to the early
Intel Mac shape.

## Things still on the list

- `gen_ovmf64.py` hardcodes the temp-RAM stack at `0x820000` instead of
  reading it from the SEC stub.
- ESP self-delete is unreliable on FAT (`0x80000003`); we zero the first 64
  bytes of our own loader instead.
- The single-character debug probes (`p`/`T`/`A`/`B`/`n`/`S`/`R`) are still
  in the trampoline — handy when bisecting, noisy otherwise.
- Inner OVMF64 `DEBUG` output goes to debugcon `0x402`, not COM1. Add
  `-debugcon file:build/debug.log -global isa-debugcon.iobase=0x402` if you
  want it.

## Be careful

- Don't install this as the primary bootloader on a machine you rely on.
- A CPU without long mode (Core Duo/Yonah) is refused cleanly — that is by
  design, not a bug.
- The 16 MiB MEMFD reservation and the fixed `0x80B000` TDX flag byte are
  tied to the firmware build. Rebuild both sides together.
- No Secure Boot guarantees.
- If it fails, send the serial log. It is the only useful bug report here.

## Credits and licensing

Our code is **MIT licensed** — see `LICENSE`. The embedded `OVMF64.fd` and
`OVMF32.fd` derive from TianoCore EDK2 (**BSD-2-Clause-Patent**); upstream
notices remain in force. The EDK2 fork is a submodule rather than a vendor
copy so provenance stays visible. See `NOTICE` for details.

Related work: TianoCore OVMF, EDK2 DUET, Intel MultiArchUefiPkg, and the
sibling `efi32boot` project, which is the sole reference implementation for
this code. This remains an AI-assisted research prototype: the claims above
come from repeatable QEMU/VMware runs, not from wishful thinking.
