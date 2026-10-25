# efi64boot — BOOTIA32.EFI: run 64-bit EFI executables from 32-bit UEFI
# firmware on a 64-bit capable CPU. AI-assisted experimental project.
#
# Usage: make TOOLCHAIN_PREFIX=/path/to/mingw/bin/ all payload

TOOLCHAIN_PREFIX ?=
CC64  := $(TOOLCHAIN_PREFIX)x86_64-w64-mingw32-gcc
LD64  := $(TOOLCHAIN_PREFIX)x86_64-w64-mingw32-ld
OBJCOPY64 := $(TOOLCHAIN_PREFIX)x86_64-w64-mingw32-objcopy
CC32  := $(TOOLCHAIN_PREFIX)i686-w64-mingw32-gcc
LD32  := $(TOOLCHAIN_PREFIX)i686-w64-mingw32-gcc
OBJCOPY32 := $(TOOLCHAIN_PREFIX)i686-w64-mingw32-objcopy
NASM  := $(TOOLCHAIN_PREFIX)nasm

CFLAGS64 := -Os -g0 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
            -fno-asynchronous-unwind-tables -fno-unwind-tables \
            -mno-red-zone -fshort-wchar -fpic -Wall -Wextra
CFLAGS32 := -Os -g0 -std=gnu11 -ffreestanding -fno-builtin -fno-stack-protector \
            -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-pic \
            -fshort-wchar -Wall -Wextra -Wno-unused-parameter

all: BOOTIA32.EFI

# ---- 64-bit payload: flat binary, position independent ----------------
build/payload64_entry.o: payload64_entry.asm
	$(NASM) -f win64 -o $@ $<

build/payload64.o: payload64.c efi32.h
	$(CC64) $(CFLAGS64) -c -o $@ $<

build/payload64.elf: build/payload64_entry.o build/payload64.o payload64.ld
	$(LD64) -T payload64.ld -o $@ build/payload64_entry.o build/payload64.o \
	    --build-id=none -static -nostdlib

build/payload64.bin: build/payload64.elf
	$(OBJCOPY64) -O binary $< $@

# ---- 32-bit trampoline: flat binary ------------------------------------
build/trampoline.bin: trampoline.asm
	$(NASM) -f bin -o $@ $<

# ---- 32-bit PE EFI application -----------------------------------------
build/blobs.o: blobs.S build/trampoline.bin build/payload64.bin
	$(CC32) $(CFLAGS32) -c -o $@ $<

build/main.o: main.c efi32.h
	$(CC32) $(CFLAGS32) -c -o $@ $<

build/efi_crt.o: efi_crt.asm
	$(NASM) -f win32 -o $@ $<

BOOTIA32.EFI: build/main.o build/blobs.o build/efi_crt.o
	$(LD32) -nostdlib -nostartfiles -nodefaultlibs \
	    -Wl,--subsystem,10 -Wl,--entry,efi_start -Wl,--no-insert-timestamp \
	    -o $@ build/main.o build/blobs.o build/efi_crt.o

payload: build/payload64.bin build/trampoline.bin

clean:
	rm -rf build BOOTIA32.EFI

.PHONY: all payload clean
