/*
 * efi64boot - BOOTIA32.EFI. 32-bit firmware that relocates the 64-bit EDK2
 * OVMF build below 4 GiB, switches the CPU to long mode and lets OVMF64
 * take over. AI-assisted experimental project, not production-safe.
 */
#include <stdint.h>
#include <stddef.h>
#include "efi32.h"
#include "ovmf64_constants.h"

#define PAYLOAD_MARKER 0x45463634

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

static void serial_putc(char c){ while ((inb(0x3FD)&0x20)==0); outb(0x3F8,(uint8_t)c); }
static void serial_puts(const char *s){ while(*s){ if(*s=='\n') serial_putc('\r'); serial_putc(*s++); } }
static void serial_hex(uint32_t v){ static const char h[]="0123456789ABCDEF"; for(int i=28;i>=0;i-=4) serial_putc(h[(v>>i)&0xF]); serial_putc(' '); }

static int cpu_has_long_mode(void){ uint32_t a,b,c,d; __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000000u)); if(a<0x80000001u)return 0; __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000001u)); return (d>>29)&1; }

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
    if (gRT) {
        typedef EFI_STATUS (*SetVariable_t)(unsigned short*, void*, unsigned int, UINTN, void*);
        unsigned short name[] = { L'B',L'o',L'o',L't',L'O',L'r',L'd',L'e',L'r', 0 };
        static const uint8_t guid[16] = { 0x61,0xdf,0xe4,0x8b, 0xca,0x93,0xd2,0x11, 0xaa,0x0d, 0x00,0xe0,0x98,0x03,0x2b,0x8c };
        unsigned short order[] = { 3, 1, 0, 2 }; /* DVD(3) first, then HDD, UiApp, Shell */
        unsigned int attrs = 0x07;
        SetVariable_t sv = (SetVariable_t)((uint32_t *)gRT)[6];
        if (sv) { EFI_STATUS rst = sv(name, (void *)guid, attrs, sizeof(order), order); serial_puts("efi64boot: Set BootOrder status "); serial_hex((uint32_t)rst); serial_puts("\n"); }
    }
    serial_puts("\nefi64boot: loading 64-bit firmware host\n");
delete_self_from_esp();
    if(!cpu_has_long_mode()){ serial_puts("efi64boot: no long mode\n"); return EFI_UNSUPPORTED; }

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