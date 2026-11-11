/* SPDX-License-Identifier: MIT */
/*
 * efi64boot - BOOTIA32.EFI. 32-bit firmware that relocates the 64-bit EDK2
 * OVMF build below 4 GiB, switches the CPU to long mode and lets OVMF64
 * take over. AI-assisted experimental project, not production-safe.
 */
#include <stdint.h>
#include <stddef.h>
#include "efi32.h"
#include "ovmf64_constants.h"
#include "platform_quirks.h"   /* canonical copy: ../shared/platform_quirks.h */

#define PAYLOAD_MARKER 0x45463634

/* efi32.h stops its EFI_MEMORY_TYPE enum at EfiRuntimeServicesData.  The
 * conventional (free RAM) type is 7 in the UEFI spec; the memory-map scan
 * below only needs the numeric value. */
#define EFI_CONVENTIONAL_MEMORY_TYPE 7u
#define EFI_MEMORY_DESCRIPTOR_TYPE_OFS   0u
#define EFI_MEMORY_DESCRIPTOR_PHYS_OFS   8u  /* EFI_PHYSICAL_ADDRESS, 8-aligned */
#define EFI_MEMORY_DESCRIPTOR_PAGES_OFS 24u

static inline void outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

extern const uint8_t  ovmf64_start[];
extern const uint32_t ovmf64_size;
extern const uint8_t  trampoline_start[];
extern const uint32_t trampoline_size;

typedef struct {
    uint32_t pml4_phys;
    uint32_t payload_phys;
    uint32_t payload_size;
    uint32_t stack_top;
    uint32_t gdt_phys;
    uint32_t serial_ok;
    uint32_t gdtr_limit, gdtr_base, idtr_limit, idtr_base;
    uint32_t cr0_32, cr3_32, cr4_32;
    uint32_t cs32, ds32, es32, ss32, fs32, gs32;
    uint32_t esp32;
    uint32_t bsv32;
    uint32_t firmware_entry;
    uint32_t firmware_fv;
    uint32_t firmware_stack;
    uint32_t idt64_phys;
    uint32_t idtr_phys;
} BOOT_PARAMS;

static EFI_SYSTEM_TABLE *gST;
static EFI_BOOT_SERVICES *gBS;
static void *gRT=0;
/* Bytes of RAM above 4 GiB as reported by QEMU/VMware CMOS 0x5B..0x5D
 * (64 KiB units).  Used as a fallback when the host UEFI memory map omits
 * high memory, which a 32-bit host firmware is allowed to do. */
static uint64_t gCmosHighMemBytes = 0;

static void serial_putc(char c){ while ((inb(0x3FD)&0x20)==0); outb(0x3F8,(uint8_t)c); }
static void serial_puts(const char *s){ while(*s){ if(*s=='\n') serial_putc('\r'); serial_putc(*s++); } }
static void serial_hex(uint32_t v){ static const char h[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) serial_putc(h[(v>>i)&0xF]); serial_putc(' '); }
static void serial_hex16(uint16_t v){ static const char h[]="0123456789ABCDEF"; for(int i=12;i>=0;i-=4) serial_putc(h[(v>>i)&0xF]); }
static void serial_hex64(uint64_t v){ static const char h[]="0123456789ABCDEF"; for(int i=60;i>=0;i-=4) serial_putc(h[(v>>i)&0xF]); }

static int cpu_has_long_mode(void){ return pq_cpu_has_long_mode(); }

/* ------------------------------------------------------------------------- */
/* Above-4G decoding                                                         */
/*                                                                           */
/* OVMF64 discovers RAM above 4 GiB from QEMU fw_cfg E820 (preferred) or, on
 * platforms without fw_cfg, from CMOS 0x5B..0x5D.  Either way its SEC/PEI
 * phase runs on OUR CR3 until DXE installs its own tables.  The original
 * build_page_tables() only identity-mapped 0..4 GiB, so any address above
 * 4 GiB faulted as soon as the guest had more than 4 GiB of RAM.
 *
 * fix: extend the initial long-mode tables to `map_top` using 1 GiB pages
 * (CPUID.80000001H:EDX[26]); fall back to 2 MiB pages on older CPUs (Core 2
 * Duo / early Intel Macs have no 1 GiB page support).  All page-table memory
 * stays below 4 GiB and is identity-mapped by PML4[0].
 * ------------------------------------------------------------------------- */

#define PQ_1GB    (1ULL << 30)
#define PQ_512GB  (1ULL << 39)
#define PQ_4GB    (1ULL << 32)

/* Highest *RAM* address (exclusive) reported by the host UEFI memory map.
 * Returns PQ_4GB when the host firmware describes no memory above 4 GiB (a
 * 32-bit host firmware is allowed to omit it).  Descriptor stride comes from
 * GetMemoryMap's DescSize -- OVMF uses 48 on X64, 40 on some IA32 builds. */
static uint64_t efi_memory_top(void) {
    typedef EFI_STATUS (*GetMemoryMap_t)(UINTN *, void *, UINTN *, UINTN *, UINT32 *);
    typedef EFI_STATUS (*AllocatePool_t)(EFI_MEMORY_TYPE, UINTN, void **);
    typedef EFI_STATUS (*FreePool_t)(void *);
    GetMemoryMap_t gmm = (GetMemoryMap_t)gBS->GetMemoryMap;
    AllocatePool_t ap  = (AllocatePool_t)gBS->AllocatePool;
    FreePool_t     fp  = (FreePool_t)gBS->FreePool;
    UINTN size = 0, key = 0, descSize = 0, actual = 0;
    UINT32 ver = 0;
    uint64_t top = PQ_4GB;
    if (!gmm || !ap) return top;
    gmm(&size, NULL, &key, &descSize, &ver);   /* expected: BUFFER_TOO_SMALL */
    if (!size || !descSize) return top;
    size += 4096;
    void *buf = NULL;
    if (EFI_ERROR(ap(EfiLoaderData, size, &buf)) || !buf) return top;
    actual = size;
    EFI_STATUS st = gmm(&actual, buf, &key, &descSize, &ver);
    if (!EFI_ERROR(st)) {
        uint8_t *p = (uint8_t *)buf;
        for (UINTN off = 0; off + descSize <= actual; off += descSize) {
            uint32_t type  = *(uint32_t *)(p + off + EFI_MEMORY_DESCRIPTOR_TYPE_OFS);
            uint64_t phys  = *(uint64_t *)(p + off + EFI_MEMORY_DESCRIPTOR_PHYS_OFS);
            uint64_t pages = *(uint64_t *)(p + off + EFI_MEMORY_DESCRIPTOR_PAGES_OFS);
            if (type != EFI_CONVENTIONAL_MEMORY_TYPE) continue;
            uint64_t end = phys + (pages << 12);
            if (end > phys && end > top) top = end;
        }
    }
    if (fp) fp(buf);
    return top;
}

/* Fill pdpt0[4..] (4 GiB..512 GiB) and, if needed, PML4[1..] with identity
 * mappings.  `pool` is a page-aligned, zeroed scratch buffer of 4 KiB pages
 * located below 4 GiB.  Returns the number of PML4 entries covered. */
static uint32_t extend_page_tables_high(uint64_t *pml4, uint64_t *pdpt0,
                                        uint8_t *pool, uint32_t pool_pages,
                                        uint64_t map_top, uint32_t phys_bits,
                                        int use_1g) {
    uint64_t cap = map_top;
    uint64_t phys_cap = (phys_bits >= 48) ? (1ULL << 48) : (1ULL << phys_bits);
    uint32_t page = 0;
    uint32_t n_pml4;

    if (cap > phys_cap) cap = phys_cap;
    /* 1 GiB pages: cap at 1 TiB.  2 MiB fallback: cap at 64 GiB so the
     * page-table pool stays small on CPUs without PDPE1GB support. */
    if (use_1g) { if (cap > (1024ULL * PQ_1GB)) cap = 1024ULL * PQ_1GB; }
    else        { if (cap >    (64ULL * PQ_1GB)) cap =   64ULL * PQ_1GB; }
    if (cap <= PQ_4GB) return 1;

    n_pml4 = (uint32_t)((cap + PQ_512GB - 1) / PQ_512GB);
    if (n_pml4 < 1) n_pml4 = 1;
    if (n_pml4 > 2) n_pml4 = 2;   /* 1 TiB with 1 GiB pages */

    /* PML4[0] -> pdpt0: entries 0..3 already cover 0..4 GiB with 2 MiB
     * pages.  Entries 4..511 cover 4 GiB..512 GiB.  A PDPT may mix PS=1
     * (1 GiB) and PS=0 (pointer to a 2 MiB PD) entries. */
    {
        uint64_t first = 4ULL * PQ_1GB;
        uint64_t last  = (cap < PQ_512GB) ? cap : PQ_512GB;
        for (uint64_t a = first; a < last; a += PQ_1GB) {
            uint32_t idx = (uint32_t)(a / PQ_1GB);
            if (use_1g) {
                pdpt0[idx] = a | 0x83u;
            } else if (page < pool_pages) {
                uint64_t *pd = (uint64_t *)(pool + (page++) * 4096u);
                pdpt0[idx] = ((uint64_t)(uint32_t)pd) | 0x03u;
                for (uint32_t k = 0; k < 512; k++)
                    pd[k] = (a + (uint64_t)k * 0x200000ULL) | 0x83u;
            } else {
                break;
            }
        }
    }

    /* PML4[1..] for address space above 512 GiB. */
    for (uint32_t i = 1; i < n_pml4; i++) {
        uint64_t *pdpt;
        uint64_t base = (uint64_t)i * PQ_512GB;
        if (page >= pool_pages) break;
        pdpt = (uint64_t *)(pool + (page++) * 4096u);
        pml4[i] = ((uint64_t)(uint32_t)pdpt) | 0x03u;
        for (uint32_t j = 0; j < 512; j++) {
            uint64_t a = base + (uint64_t)j * PQ_1GB;
            if (a >= cap) break;
            if (use_1g) {
                pdpt[j] = a | 0x83u;
            } else if (page < pool_pages) {
                uint64_t *pd = (uint64_t *)(pool + (page++) * 4096u);
                pdpt[j] = ((uint64_t)(uint32_t)pd) | 0x03u;
                for (uint32_t k = 0; k < 512; k++)
                    pd[k] = (a + (uint64_t)k * 0x200000ULL) | 0x83u;
            } else {
                break;
            }
        }
    }
    return n_pml4;
}

static void build_page_tables(uint64_t *pml4, uint64_t *pdpt, uint64_t *pds){ for(int i=0;i<512;i++){pml4[i]=0;pdpt[i]=0;} pml4[0]=((uint64_t)(uint32_t)pdpt)|0x03; for(int g=0;g<4;g++){ uint64_t *pd=pds+g*512; pdpt[g]=((uint64_t)(uint32_t)pd)|0x03; for(int i=0;i<512;i++) pd[i]=((((uint64_t)g*512u)+(uint64_t)i)<<21)|0x83; } }

static void relocate(uint8_t *image){ UINT8 *sec=image+OVMF64_SEC_FILE_OFFSET; long long delta=(long long)(UINTN)sec - (long long)OVMF64_SEC_LINK_BASE; UINT8 *r=image+OVMF64_SEC_RELOC_RVA; UINTN cursor=0; while (cursor+8<=OVMF64_SEC_RELOC_SIZE){ UINT32 page=*(UINT32*)(r+cursor); UINT32 bs=*(UINT32*)(r+cursor+4); if(bs<8 || cursor+bs>OVMF64_SEC_RELOC_SIZE)break; UINTN n=(bs-8)/2; for(UINTN i=0;i<n;i++){ UINT16 e=*(UINT16*)(r+cursor+8+i*2); UINTN type=e>>12; UINTN ofs=e&0xFFF; if(type==10){ unsigned long long *slot=(unsigned long long*)(sec+page+ofs); *slot=(unsigned long long)((long long)*slot+delta); } else {} } cursor+=bs; } serial_puts("efi64boot: SEC rebased, delta="); serial_hex((uint32_t)delta); serial_puts("\n"); }

static void delete_self_from_esp(void) {
    typedef EFI_STATUS (*LocateHandleBuffer_t)(UINTN, void *, void *, UINTN *, EFI_HANDLE **);
    typedef EFI_STATUS (*HandleProtocol_t)(EFI_HANDLE, void *, void **);
    static const uint8_t sfs_guid[16] = { 0x22,0x5b,0x4e,0x96, 0x59,0x64, 0xd2,0x11,0x8e,0x39, 0x00,0xa0,0xc9,0x69,0x72,0x3b };
    LocateHandleBuffer_t lhb = (LocateHandleBuffer_t)gBS->LocateHandleBuffer;
    HandleProtocol_t hp = (HandleProtocol_t)gBS->HandleProtocol;
    UINTN n = 0;
    EFI_HANDLE *buf = NULL;
    if (lhb == NULL || hp == NULL) return;
    EFI_STATUS lhs = lhb(2, (void *)sfs_guid, NULL, &n, &buf);
    serial_puts("efi64boot: delete_self LocateHandleBuffer status "); serial_hex((uint32_t)lhs); serial_puts(" count "); serial_hex((uint32_t)n); serial_puts("\n");
    if (EFI_ERROR(lhs)) { return; }
    for (UINTN i = 0; i < n; i++) {
        void *fs = NULL;
        EFI_STATUS hst = hp(buf[i], (void *)sfs_guid, &fs);
        if (EFI_ERROR(hst) || fs == NULL) { serial_puts("efi64boot: delete_self handleprotocol status "); serial_hex((uint32_t)hst); serial_puts("\n"); continue; }
        void *root = NULL;
        if (EFI_ERROR(((EFI_STATUS (*)(void *, void **))(*(uint32_t *)((uint8_t *)fs + 8)))(fs, &root)) || root == NULL) continue;
        serial_puts("efi64boot: delete_self root opened\n");
        unsigned short path[] = { 0x5C,0x5C,'E','F','I',0x5C,'B','O','O','T',0x5C,'B','O','O','T','I','A','3','2','.','E','F','I',0 };
        void *fh = NULL;
        EFI_STATUS st = ((EFI_STATUS (*)(void *, void **, void *, uint64_t, uint64_t))(((uint32_t *)root)[2]))(root, &fh, path, 3, 0);
        serial_puts("efi64boot: delete_self open BOOTIA32 status "); serial_hex((uint32_t)st); serial_puts("\n");
        if (!EFI_ERROR(st) && fh != NULL) {
            UINT8 zero[64]; for (int _z=0;_z<64;_z++) zero[_z]=0;
            UINTN wsize = 64;
            EFI_STATUS wst = ((EFI_STATUS (*)(void *, UINTN *, void *))(((uint32_t *)fh)[6]))(fh, &wsize, zero);
            serial_puts("efi64boot: delete_self write0 status "); serial_hex((uint32_t)wst); serial_puts(" wrote "); serial_hex((uint32_t)wsize); serial_puts("\n");
            EFI_STATUS dst = ((EFI_STATUS (*)(void *))(((uint32_t *)fh)[4]))(fh);
            serial_puts("efi64boot: delete_self delete status "); serial_hex((uint32_t)dst); serial_puts("\n");
            if (EFI_ERROR(dst)) {
                EFI_STATUS cst = ((EFI_STATUS (*)(void *))(((uint32_t *)fh)[3]))(fh);
                serial_puts("efi64boot: delete_self close status "); serial_hex((uint32_t)cst); serial_puts("\n");
            }
        }
        EFI_STATUS rst = ((EFI_STATUS (*)(void *))(((uint32_t *)root)[3]))(root);
    }
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab){
    BOOT_PARAMS *params; uint8_t *region; uint64_t *pml4,*pdpt,*pds,*gdt64;
    gST=systab; gBS=systab->BootServices; gRT=(void*)systab->RuntimeServices;
    /* Detect the platform once, correctly.  A 32-bit read at PCI config
     * offset 0 is VID in the low word / DID in the high word; the old code
     * printed the raw dword and compared it against a byte-swapped constant,
     * then tried to overwrite the (read-only) DID.  See platform_quirks.h. */
    pq_platform_info_t plat = pq_platform_detect();
    {
        uint32_t cfg_units = (uint32_t)(
            ((uint64_t)pq_cmos_read(0x5Du) << 16) |
            ((uint64_t)pq_cmos_read(0x5Cu) <<  8) |
            (uint64_t)pq_cmos_read(0x5Bu));
        gCmosHighMemBytes = (uint64_t)cfg_units << 16;
    }
    serial_puts("efi64boot: platform ");
    serial_puts(plat.name);
    serial_puts("  host bridge ");
    serial_hex16(plat.vid); serial_putc(':'); serial_hex16(plat.did);
    serial_puts("  (cfg dword "); serial_hex(plat.raw); serial_puts(")\n");
    if (plat.pm_present) {
        serial_puts("efi64boot: ACPI PM device 00:");
        serial_hex16(plat.pm_dev); serial_putc('.'); serial_putc('0' + plat.pm_fn);
        serial_puts(" class "); serial_hex16(plat.pm_class);
        serial_puts(" style "); serial_hex((uint32_t)plat.pm_style);
        serial_puts("\n");
    } else {
        serial_puts("efi64boot: WARNING no ACPI PM device (class 0680/0601) found\n");
    }
    if (plat.generic_fallback) {
        serial_puts("efi64boot: host bridge has no dedicated OVMF path; using the\n");
        serial_puts("efi64boot: generic fallback in the single universal firmware build\n");
    }
    /* No BootOrder / Boot#### variable manipulation.  The old code forced
     * "DVD first, then HDD, UiApp, Shell" to make Windows Setup boot first;
     * that is an OS-specific policy and breaks any other EFI OS.  The inner
     * firmware's BDS owns boot policy entirely. */
    (void)gRT;
    serial_puts("\nefi64boot: loading 64-bit firmware host\n");
delete_self_from_esp();
    /* Legal platform quirk: make sure the ACPI PM I/O space is enabled on
     * the *actual* PM device (probed, not assumed).  Stock OVMF64 programs
     * PMBA at 00:01.3; VMware's PIIX4 PM is at 00:07.3 and ICH7/ICH9 LPC
     * at 00:1F.0, so the loader does it here before handoff.  This is a
     * no-op if the host firmware already enabled it (the normal case),
     * because reprogramming PMBASE under a running firmware breaks SMI/SCI. */
    if (pq_enable_pm_io(&plat))
        serial_puts("efi64boot: ACPI PM I/O space enabled on probed PM device\n");

    if(!cpu_has_long_mode()){
        serial_puts("efi64boot: CPU has no IA-32e long mode - cannot host OVMF64\n");
        if (plat.kind == PQ_PLAT_INTEL_MOBILE || plat.kind == PQ_PLAT_APPLE_INTEL)
            serial_puts("efi64boot: 2005/2006 Core Duo (Yonah) Macs are 32-bit only; Core 2 Duo required\n");
        return EFI_UNSUPPORTED;
    }

    UINTN ipages=(OVMF64_IMAGE_SIZE+4095)/4096;
    EFI_PHYSICAL_ADDRESS addr=0;
    EFI_STATUS status=gBS->AllocatePages(AllocateAnyPages,EfiLoaderData,ipages,&addr);
    if(EFI_ERROR(status)||addr>>40ULL){ serial_puts("efi64boot: firmware alloc failed\n"); return status; }
    for(uint32_t i=0;i<OVMF64_IMAGE_SIZE;i++) ((uint8_t*)(UINTN)addr)[i]=ovmf64_start[i];
    relocate((uint8_t*)(UINTN)addr);

    /* OVMF64 is built for a fixed MEMFD region at 0x800000 (see OvmfPkgX64.fdf
     * [FD.MEMFD]: BaseAddress = MEMFD_BASE_ADDRESS = 0x800000, Size = 0xF80000).
     * The SEC entry stub uses PcdOvmfWorkAreaBase = 0x80B000, temp RAM at
     * 0x814000..0x820000, DecompressMemFvs copies PEIFV to 0x820000 and
     * DXEFV to 0x100000, and the LZMA scratch reaches 0x1E80000.  The whole
     * region must be reserved at its build address so no PCD patching is
     * needed (same principle as efi32boot's OVMF32_MEMFD_BASE/Size). */
    UINTN pages=0xF80000/4096;
    EFI_PHYSICAL_ADDRESS raddr=0x00800000ULL;
    status=gBS->AllocatePages(2,EfiLoaderData,pages,&raddr);   /* AllocateAddress */
    if(EFI_ERROR(status)){ raddr=0; status=gBS->AllocatePages(AllocateAnyPages,EfiLoaderData,pages,&raddr); }
    if(EFI_ERROR(status)||raddr>>40ULL){ serial_puts("efi64boot: MEMFD alloc failed\n"); return status; }
    serial_puts("efi64boot: MEMFD reserved at "); serial_hex((uint32_t)raddr); serial_puts("\n");
    region=(uint8_t*)(uint32_t)raddr;
    /* Zero the whole staging region: OVMF64 SEC branches on the byte at
     * 0x80B000, which must be 0 so the native (non-TDX) path is taken. */
    for (UINTN zi=0; zi < (UINTN)pages*4096; zi++) region[zi] = 0;
    /* Leave 0x80B000 at its zero/default value so OVMF64 follows the
     * native (non-TDX) SEC path. Forcing 2 selected the TDX tdcall path. */
    pml4=(uint64_t*)(region+0x00000); pdpt=(uint64_t*)(region+0x01000); pds=(uint64_t*)(region+0x02000); gdt64=(uint64_t*)(region+0x06000);
    params=(BOOT_PARAMS*)(region+0x07000);
    build_page_tables(pml4,pdpt,pds);

    /* ---- above-4G decoding ------------------------------------------- */
    {
        uint64_t map_top   = efi_memory_top();
        uint64_t cmos_top  = PQ_4GB + gCmosHighMemBytes;
        uint64_t high_top  = (map_top > cmos_top) ? map_top : cmos_top;
        uint64_t map_limit = high_top + 64ULL * PQ_1GB; /* room for MMIO64 */
        uint32_t phys_bits = pq_cpu_phys_bits();
        int      use_1g    = pq_cpu_has_1g_pages();
        uint32_t pool_pages = use_1g ? 2u : 68u;
        EFI_PHYSICAL_ADDRESS pool_addr = 0;
        uint8_t *pool = 0;

        serial_puts("efi64boot: RAM top "); serial_hex64(high_top);
        serial_puts(" CMOS-high "); serial_hex64(gCmosHighMemBytes);
        serial_puts(" physbits "); serial_hex(phys_bits);
        serial_puts(" PDPE1GB "); serial_hex((uint32_t)use_1g);
        serial_puts("\n");

        /* Publish high RAM to OVMF64's CMOS fallback path.  No platform
         * allowlist: pq_cmos_set_highmem() probes that 0x5B..0x5D behave as
         * scratch RAM, restores the originals on failure, and only then
         * commits.  Machines whose EC/SMM owns those bytes are left alone. */
        if (high_top > PQ_4GB) {
            if (pq_cmos_set_highmem(high_top - PQ_4GB))
                serial_puts("efi64boot: CMOS 0x5B..0x5D high-memory size published\n");
            else
                serial_puts("efi64boot: CMOS high-mem bytes not writable; skipped (no platform-specific build needed)\n");
        }

        if (high_top > PQ_4GB) {
            EFI_STATUS pst = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                                pool_pages, &pool_addr);
            if (!EFI_ERROR(pst) && pool_addr < PQ_4GB) {
                pool = (uint8_t *)(uint32_t)pool_addr;
                for (UINTN z = 0; z < (UINTN)pool_pages * 4096u; z++) pool[z] = 0;
                extend_page_tables_high(pml4, pdpt, pool, pool_pages,
                                        map_limit, phys_bits, use_1g);
                serial_puts("efi64boot: identity map extended above 4 GiB to ");
                serial_hex64(map_limit);
                serial_puts("\n");
            } else {
                serial_puts("efi64boot: WARNING high-memory page-table pool alloc failed; above-4G NOT mapped\n");
            }
        }
    }

    gdt64[0]=0; gdt64[1]=0x00AF9A000000FFFFULL; gdt64[2]=0x00CF92000000FFFFULL; gdt64[3]=0x00CF9A000000FFFFULL; gdt64[4]=0x00CF9A000000FFFFULL;

    params->pml4_phys=(uint32_t)(uint32_t*)pml4;
    params->stack_top=(uint32_t)(region+0x1F000);
    params->gdt_phys=(uint32_t)(uint32_t*)gdt64;
    params->serial_ok=0;
    params->bsv32=(uint32_t)(uint32_t*)gBS;
    params->firmware_entry=(uint32_t)((uint8_t*)(UINTN)addr+OVMF64_SEC_FILE_OFFSET+OVMF64_SEC_ENTRY_RVA);
    params->firmware_fv=(uint32_t)((uint8_t*)(UINTN)addr+OVMF64_SEC_FV_OFFSET);
    params->firmware_stack=OVMF64_TEMP_RAM_STACK;
    params->idt64_phys=(uint32_t)(region+0x09000);
    params->idtr_phys=(uint32_t)(region+0x0A000);

    uint8_t *tramp_dst=region+0x08000;
    for(uint32_t i=0;i<trampoline_size;i++) tramp_dst[i]=trampoline_start[i];
    serial_puts("efi64boot: firmware at "); serial_hex((uint32_t)addr); serial_puts(" entry "); serial_hex(params->firmware_entry); serial_puts("\n");

    /* Create handoff marker file so a re-invocation (BdsDxe retry) is blocked. */
{
    typedef EFI_STATUS (*LocateHandleBuffer_t)(UINTN, void *, void *, UINTN *, EFI_HANDLE **);
    typedef EFI_STATUS (*HandleProtocol_t)(EFI_HANDLE, void *, void **);
    static const uint8_t sfs_guid[16] = { 0x22,0x5b,0x4e,0x96, 0x59,0x64, 0xd2,0x11,0x8e,0x39, 0x00,0xa0,0xc9,0x69,0x72,0x3b };
    LocateHandleBuffer_t lhb = (LocateHandleBuffer_t)gBS->LocateHandleBuffer;
    HandleProtocol_t hp = (HandleProtocol_t)gBS->HandleProtocol;
    UINTN n = 0; EFI_HANDLE *buf = NULL;
    if (lhb && hp && !EFI_ERROR(lhb(2, (void *)sfs_guid, NULL, &n, &buf)) && n) {
        for (UINTN i = 0; i < n; i++) {
            void *fs = NULL;
            if (EFI_ERROR(hp(buf[i], (void *)sfs_guid, &fs)) || !fs) continue;
            void *root = NULL;
            if (EFI_ERROR(((EFI_STATUS (*)(void *, void **))(*(uint32_t *)((uint8_t *)fs + 8)))(fs, &root)) || !root) continue;
            unsigned short mpath[] = { 0x5C,0x5C,'E','F','I',0x5C,'B','O','O','T',0x5C,'E','F','6','4','D','O','N','E',0 };
            void *fh = NULL;
            EFI_STATUS mst = ((EFI_STATUS (*)(void *, void **, void *, uint64_t, uint64_t))(((uint32_t *)root)[2]))(root, &fh, mpath, 0x80000000ULL | 3ULL, 0);
            if (!EFI_ERROR(mst) && fh) {
                ((EFI_STATUS (*)(void *))(((uint32_t *)fh)[3]))(fh);
                serial_puts("efi64boot: handoff marker created\n");
            }
            ((EFI_STATUS (*)(void *))(((uint32_t *)root)[3]))(root);
            break;
        }
    }
}
typedef EFI_STATUS (*GetMemoryMap_t)(UINTN*, void*, UINTN*, UINTN*, UINT32*);
    typedef EFI_STATUS (*ExitBootServices_t)(EFI_HANDLE, UINTN);
    UINTN mapSize=0, DescSize=0, MapKey=0, DescVersion=0;
    GetMemoryMap_t gmm=(GetMemoryMap_t)gBS->GetMemoryMap;
    ExitBootServices_t ebs=(ExitBootServices_t)gBS->ExitBootServices;
    gmm(&mapSize,NULL,&MapKey,&DescSize,&DescVersion);
    mapSize+=8192;
    void *map;
    ((EFI_STATUS (*)(EFI_MEMORY_TYPE,UINTN,void**))gBS->AllocatePool)(EfiLoaderData,mapSize,&map);
    status=gmm(&mapSize,map,&MapKey,&DescSize,&DescVersion);
    if(!EFI_ERROR(status)){ status=ebs(image,MapKey); serial_puts("efi64boot: ExitBootServices status "); serial_hex((uint32_t)status); serial_puts("\n"); }

    void (*tramp)(BOOT_PARAMS *)=(void (*)(BOOT_PARAMS *))(region+0x08000);
    tramp(params);
    return EFI_SUCCESS;
}