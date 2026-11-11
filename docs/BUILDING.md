# Building efi64boot

Two minutes for the loader, a few more if you want to rebuild the embedded
64-bit firmware from source.

## What you need

- Python 3
- `make`
- MinGW-w64 cross compilers (`i686-w64-mingw32-gcc` and
  `x86_64-w64-mingw32-gcc`)
- NASM
- `mtools` (for the QEMU tests)

The sibling repo has a helper that installs a private toolchain:

```bash
git clone https://github.com/devtinoimao/efi32boot.git
cd efi32boot && ./setup_toolchain.sh ./toolchain
export PATH="$PWD/toolchain/usr/bin:$PATH"
```

Or just use your system MinGW-w64 and NASM.

## 1. Build the loader

```bash
git clone --recurse-submodules https://github.com/devtinoimao/efi64boot.git
cd efi64boot
make all                 # -> BOOTIA32.EFI
```

`OVMF64.fd` is checked in and `ovmf64_constants.h` was generated from it, so
EDK2 is not needed for this step. If you have your own toolchain in a
non-standard place:

```bash
make TOOLCHAIN_PREFIX=/path/to/mingw/bin/ all
```

Avoid spaces in `TOOLCHAIN_PREFIX` — the Makefile expands it unquoted. If the
path has spaces, put the directory on `PATH` and leave the prefix empty.

## 2. Rebuild the embedded firmware (optional)

The `edk2` submodule points at the `hosted-firmware` branch of
[devtinoimao/edk2](https://github.com/devtinoimao/edk2/tree/hosted-firmware).
The X64 build needs `MEMFD_BASE_ADDRESS = 0x800000`, while the IA32 build in
the sibling repo needs `0x40000000`; `shared/rebuild_ovmf.sh` switches the
shared FDF include for the X64 pass and restores it on exit.

```bash
git submodule update --init --depth 1 edk2   # if not cloned recursively
bash shared/rebuild_ovmf.sh
```

Builds run inside `localhost/edk2-builder:bookworm` (GCC 12, NASM, iasl,
EDK2 BaseTools). The script copies `OVMF_CODE.fd` to `OVMF64.fd`, regenerates
`ovmf64_constants.h` with `tools/gen_ovmf64.py`, and relinks the loader.

Always regenerate the constants header after a firmware rebuild — the SEC
file offset, entry RVA and relocation directory move when the source changes.

## 3. Check that it works

See the README for the exact QEMU commands. The short version:

```bash
# build an ESP containing only BOOTIA32.EFI, then:
qemu-system-x86_64 -accel tcg -cpu qemu64 -m 2048 -smp 2 \
  -bios OVMF32.fd \
  -drive format=raw,file=build/esp.img \
  -serial file:build/boot.log -display none -no-reboot
```

Look for `pTAB0n` (long mode reached, SEC called) followed by `BdsDxe` lines.
`XCnn` means the trampoline's per-vector watchdog caught an exception — the
two hex digits are the vector, which is usually enough to find the cause.

To boot an actual OS, attach an ISO you provide yourself with `-cdrom`.

## If something breaks

Send `build/boot.log`. The loader logs the platform, the host-bridge ID, the
ACPI PM device, RAM top, physical-address width, the 1 GiB-page capability
and the firmware relocation delta before handing off. Inner OVMF64 `DEBUG`
output goes to debugcon `0x402`, not COM1 — add
`-debugcon file:build/debug.log -global isa-debugcon.iobase=0x402` to capture
it too.

Real hardware is unvalidated; read the safety notes in the README first.
