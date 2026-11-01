# EFI64Boot Development Context — Saved State (2026-11-02)

## Recent progress
- Watchdog added to efi64boot: replaced the silent `ign_ir` (iretq) IDT handler with per-vector fault stubs (one per vector, push vector + jmp to `fault_common`) that print "XC <vector>" then halt with 'Z' loop on any fault
- Every fault (including General Protection #GP = vector 0x0D) is now visible on COM1
- Verified: build succeeds and `pTA` probes land normally, BdsDxe still reached (build confirms IDT region valid)

## Objective
Make efi64boot (32-bit IA32 `BOOTIA32.EFI`) hand off to a hosted 64-bit OVMF64 SEC entry and boot `win10ltc.iso`, using efi32boot as the sole reference repository.

## Status
- OVMF64 handoff proven (M3): TCG+qemu64 and KVM+core2duo both reach BdsDxe, Windows Setup renders on VGA
- MTRR assert (the previous blocker) fixed by setting IA32_MTRR_DEF_TYPE = 0x806 before SEC call
- Builds clean (efi64boot `BOOTIA32.EFI`, efi32boot `BOOTX64.EFI`); both tagged v1.0 and pushed

## Files changed since last context
- `efi64boot/trampoline.asm`: MTRR hygiene + per-vector fault watchdog (lines ~143-195, 248-275)
- `efi64boot/main.c`: MEMFD 32→16MB (line 116-125)

## Remaining housekeeping (not blockers)
- ESP self-delete still fails (FAT driver 0x80000003/0x00000002); handled via zeroing first 64 bytes
- Debug probes (p/T/A/B/n/S/R/Z) still present
- Serial output during OVMF64 SEC/DXE is via debugcon 0x402; capture with `-debugcon`
- OVMF64 side: user lands on "Press any key to boot from CD or DVD" — keypress required

## Media (must exist per-task)
- VM media: `/home/abokaborda/w64work/win10ltc.iso`
- ESP base: `/home/abokaborda/w64work/esp_m2.img`

## Reference: EFI32Boot (sole source of truth)
- `main.c`: PrepareHostedFirmware, OVMF32_MEMFD_BASE/Size, AllocateBelow4G, DeleteEspFile, ExitBootServices, Mark()
- `transition.asm`: handover page, GDT, far jmp, CR0/CR4/MTRR/EFER hygiene
- `tools/gen_ovmf32.py`: SEC entry stub scanner
- `build/ovmf32_constants.h`: generated constants shape

## Forum drafts ready (in /tmp)
- `/tmp/winraid_post.txt`, `/tmp/reboot_pro_post.txt`, `/tmp/mydigitallife_post.txt`, `/tmp/betaarchive_post.txt`
- See README updated (`efi64boot/README.md`) summarizing M3 milestone