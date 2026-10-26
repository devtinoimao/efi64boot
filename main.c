/*
 * efi64boot — BOOTIA32.EFI: launch 64-bit EFI executables from 32-bit
 * UEFI firmware when the CPU itself is 64-bit capable.
 *
 * Milestone 1 (this file): CPUID gate, low-memory allocation for page
 * tables + 64-bit payload, identity page table build, and the
 * 32-bit -> long mode handoff. The M1 payload prints to COM1 directly
 * (no firmware services on the 64-bit side yet).
 *
 * AI-assisted experimental firmware. Not production-safe. See docs/.
 */

#include <stdint.h>
#include "efi32.h"

#define PAYLOAD_MARKER 0x45463634 /* "EF64" */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* From the linker script / blob */
extern const uint8_t  payload64_start[];
extern const uint32_t payload64_size;
extern const uint8_t  trampoline_start[];
extern const uint32_t trampoline_size;

/* Layout handed to the trampoline (must stay in sync with trampoline.asm).
 * M2: the trampoline captures the 32-bit firmware CPU state pre-switch so
 * the 64-bit thunk layer can drop back down for service calls. */
typedef struct {
    uint32_t pml4_phys;      /* PML4 table, 4K aligned            */
    uint32_t payload_phys;   /* 64-bit payload entry base         */
    uint32_t payload_size;
    uint32_t stack_top;      /* 64-bit stack (grows down)         */
    uint32_t gdt_phys;       /* 64-bit GDT                        */
    uint32_t serial_ok;      /* out: payload sets 0x600D when done */
    /* M2 state capture (filled by trampoline pre-switch) */
    uint32_t gdtr_limit;     /* 32-bit GDTR limit                 */
    uint32_t gdtr_base;      /* 32-bit GDTR base (<4GiB)          */
    uint32_t idtr_limit;
    uint32_t idtr_base;
    uint32_t cr0_32, cr3_32, cr4_32;
    uint32_t cs32, ds32, es32, ss32, fs32, gs32;
    uint32_t esp32;          /* 32-bit stack pointer at capture   */
    uint32_t bsv32;          /* EFI_BOOT_SERVICES* (32-bit)       */
} BOOT_PARAMS;

static EFI_SYSTEM_TABLE *gST;
static EFI_BOOT_SERVICES *gBS;

/* ------------------------------------------------------------------ */
/* Serial COM1 direct output (works on both sides, 32-bit here)        */
static void serial_putc(char c)
{
    while ((inb(0x3FD) & 0x20) == 0)
        ;
    outb(0x3F8, (uint8_t)c);
}

static void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_hex(uint32_t v)
{
    static const char h[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 28; i >= 0; i -= 4)
        serial_putc(h[(v >> i) & 0xF]);
}

/* ------------------------------------------------------------------ */
static int cpu_has_long_mode(void)
{
    uint32_t a, b, c, d;
    /* CPUID(0x80000000): max extended leaf */
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(0x80000000u));
    if (a < 0x80000001u)
        return 0;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(0x80000001u));
    return (d >> 29) & 1; /* LM bit */
}

/* ------------------------------------------------------------------ */
/* Identity-map the first 4 GiB with 2 MiB pages: PML4[0] -> PDPT[0..3]*/
/* -> PD[0..3][0..511]. The firmware, our stack and the staging region */
/* all live near 2 GiB, so a 1 GiB map is NOT enough (learned the hard */
/* way: triple fault on the first mov cr3).                            */
static void build_page_tables(uint64_t *pml4, uint64_t *pdpt, uint64_t *pds)
{
    for (int i = 0; i < 512; i++) {
        pml4[i] = 0;
        pdpt[i] = 0;
    }
    pml4[0] = ((uint64_t)(uint32_t)pdpt) | 0x03; /* present | writable */
    for (int g = 0; g < 4; g++) {
        uint64_t *pd = pds + (uint32_t)g * 512;
        pdpt[g] = ((uint64_t)(uint32_t)pd) | 0x03;
        for (int i = 0; i < 512; i++)
            pd[i] = ((((uint64_t)g * 512u) + (uint64_t)i) << 21) | 0x83;
    }
}

/* ------------------------------------------------------------------ */
EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    EFI_STATUS status;
    BOOT_PARAMS *params;
    uint8_t *region;
    uint64_t *pml4, *pdpt, *pds;
    uint64_t *gdt64;

    gST = systab;
    gBS = systab->BootServices;

    serial_puts("\nefi64boot: BOOTIA32.EFI on 32-bit firmware\n");
    serial_puts("efi64boot: firmware HandleProtocol+ at ST=0x");
    serial_hex((uint32_t)(uint32_t *)systab);
    serial_puts("\n");
    serial_puts("efi64boot: gBS=0x");
    serial_hex((uint32_t)(uint32_t *)gBS);
    serial_puts(" AllocatePages=0x");
    serial_hex((uint32_t)(uint32_t *)gBS->AllocatePages);
    serial_puts("\n");

    /* 1. Is the CPU actually 64-bit? If not, this whole thing is moot. */
    if (!cpu_has_long_mode()) {
        serial_puts("efi64boot: FATAL - CPU has no long mode; cannot run 64-bit code\n");
        return EFI_UNSUPPORTED;
    }
    serial_puts("efi64boot: CPUID 0x80000001.LM = 1 - 64-bit capable CPU on 32-bit firmware\n");

    /* 2. Allocate one contiguous region below 4 GiB:
     *    [pml4 4K][pdpt 4K][pd 4K][gdt 4K][payload+trampoline ...][stack 64K] */
    {
        UINTN pages = 32; /* 128 KiB is plenty for M1 */
        EFI_PHYSICAL_ADDRESS addr = 0;

        serial_puts("efi64boot: calling AllocatePages...\n");
        status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                    pages, &addr);
        serial_puts("efi64boot: back, status=0x");
        serial_hex((uint32_t)status);
        serial_puts("\n");
        if (EFI_ERROR(status)) {
            serial_puts("efi64boot: AllocatePages failed\n");
            return status;
        }
        region = (uint8_t *)(uint32_t)addr;
        serial_puts("efi64boot: staging region at ");
        serial_hex((uint32_t)addr);
        serial_puts("\n");
    }

    pml4   = (uint64_t *)(region + 0x00000);
    pdpt   = (uint64_t *)(region + 0x01000);
    pds    = (uint64_t *)(region + 0x02000);   /* 4 page dirs: 0x2000-0x5FFF */
    gdt64  = (uint64_t *)(region + 0x06000);
    params = (BOOT_PARAMS *)(region + 0x07000);

    build_page_tables(pml4, pdpt, pds);

    /* 64-bit GDT: null, code64 (0x08), data (0x10, unused but sane) */
    {
        volatile uint64_t *g = gdt64;
        g[0] = 0;
        g[1] = 0x00AF9A000000FFFFULL; /* 64-bit code, L=1, present, ring0 */
        g[2] = 0x00CF92000000FFFFULL; /* data */
        g[3] = 0x00CF9A000000FFFFULL; /* 32-bit compat code (L=0, D=1) at
                                         selector 0x18 — target of the
                                         thunk's long→compat far return
                                         (plan B2) */
        g[4] = 0x00CF9A000000FFFFULL; /* 32-bit code again (L=0, D=1) at
                                         selector 0x20 — target of the in_compat
                                         4+4 retf to in_32bit (same bridge GDT,
                                         no firmware-GDT swap needed) */
    }

    params->pml4_phys    = (uint32_t)(uint32_t *)pml4;
    params->stack_top    = (uint32_t)(region + 0x1F000);
    params->gdt_phys     = (uint32_t)(uint32_t *)gdt64;
    params->serial_ok    = 0;
    params->bsv32        = (uint32_t)(uint32_t *)gBS;

    /* 3. Copy the 64-bit payload and the 32-bit trampoline into the
     *    staging region. Trampoline sits at region+0x05000 and is entered
     *    with a plain 32-bit call — paging setup happens there. */
    {
        uint8_t *payload_dst = region + 0x09000;
        uint8_t *tramp_dst   = region + 0x08000;

        for (uint32_t i = 0; i < payload64_size; i++)
            payload_dst[i] = payload64_start[i];
        for (uint32_t i = 0; i < trampoline_size; i++)
            tramp_dst[i] = trampoline_start[i];

        params->payload_phys = (uint32_t)(uint32_t *)payload_dst;
        params->payload_size = payload64_size;
    }

    serial_puts("efi64boot: payload at ");
    serial_hex(params->payload_phys);
    serial_puts(", trampoline at ");
    serial_hex((uint32_t)(uint32_t *)(region + 0x08000));
    serial_puts(", switching to long mode\n");

    /* 4. Call the 32-bit trampoline. It never returns on M1
     *    (the 64-bit payload dead-loops after reporting). */
    {
        void (*tramp)(BOOT_PARAMS *) =
            (void (*)(BOOT_PARAMS *))(region + 0x08000);
        tramp(params);
    }

    /* Not reached in M1 */
    serial_puts("efi64boot: trampoline returned (unexpected)\n");
    return EFI_SUCCESS;
}
