/*
 * efi64boot M1 64-bit payload — runs in long mode on a CPU that the
 * 32-bit firmware only drives in protected mode. Prints proof-of-life
 * to COM1 directly (no UEFI services on the 64-bit side yet), sets the
 * done marker, and dead-loops (clean return-to-32 is M2).
 *
 * Compiled freestanding x86_64, linked at its staging base.
 */

#include <stdint.h>

#define PARAMS_MAGIC 0x45463634ULL /* "EF64" */

typedef struct {
    uint64_t pml4_phys;
    uint64_t payload_phys;
    uint64_t payload_size;
    uint64_t stack_top;
    uint64_t gdt_phys;
    volatile uint64_t serial_ok;
} PAYLOAD_PARAMS;

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

static void serial_hex(uint64_t v)
{
    static const char h[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putc(h[(v >> i) & 0xF]);
}

/* M2 test: round-trip a firmware service through the thunk layer.
 * We call Stall(1 second) via the thunked svc table and report. */
extern void fake64_init(void *BootServices32);
extern void fake64_bind_services(void);
extern uint32_t svc_table32[64];
extern unsigned char saved_gdtr32[10];
extern unsigned char saved_idtr32[10];
extern uint32_t saved_cr0_32, saved_cr3_32, saved_cr4_32;
extern uint32_t saved_ds32, saved_es32, saved_fs32, saved_gs32;
extern uint32_t saved_ss32, saved_cs32;
extern uint32_t thunk_stack32_top;
extern uint64_t do_thunk_call(uint64_t *args, uint64_t *retout,
                              uint32_t svc_index, uint32_t nargs);
#define SVC_STALL 28

/* Entry: the trampoline calls here with RCX = PAYLOAD_PARAMS* (MS x64 ABI,
 * mingw build — we control both sides). */
void payload64_main(PAYLOAD_PARAMS *params)
{
    uint32_t lo, hi;

    serial_puts("\nefi64boot: *** HELLO FROM 64-BIT LONG MODE ***\n");
    serial_puts("efi64boot: running 64-bit code hosted by 32-bit firmware\n");

    /* double-check where we are */
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    serial_puts("efi64boot: IA32_EFER = ");
    serial_hex(((uint64_t)hi << 32) | lo);
    serial_puts("\n");

    __asm__ volatile("mov %%cr4, %%rax" : "=a"(lo));
    serial_puts("efi64boot: CR4 = ");
    serial_hex(lo);
    serial_puts("\n");

    if (params && params->serial_ok == 0) {
        params->serial_ok = 0x600D; /* "good" */
        serial_puts("efi64boot: M1 marker set - success\n");
    }

    /* ---- M2: install the 32-bit state and round-trip a service ---- */
    if (params) {
        uint32_t *p32 = (uint32_t *)params;
        unsigned char *pr = (unsigned char *)params;
        /* 10-byte layout for the 64-bit LGDT/LIDT: limit(2) + base(8) */
        {
            unsigned char *g = (unsigned char *)saved_gdtr32;
            unsigned char *i = (unsigned char *)saved_idtr32;
            /* sgdt/sidt store packed: limit(2 bytes) then base(4 bytes).
             * params+24 = gdtr blob, params+32 = idtr blob. */
            {
                unsigned char *gd = (unsigned char *)params + 24;
                unsigned char *id = (unsigned char *)params + 32;
                g[0] = gd[0]; g[1] = gd[1];
                g[2] = gd[2]; g[3] = gd[3]; g[4] = gd[4]; g[5] = gd[5];
                g[6] = g[7] = g[8] = g[9] = 0;
                i[0] = id[0]; i[1] = id[1];
                i[2] = id[2]; i[3] = id[3]; i[4] = id[4]; i[5] = id[5];
                i[6] = i[7] = i[8] = i[9] = 0;
            }
        }
        saved_cr0_32 = p32[10];
        saved_cr3_32 = p32[11];
        saved_cr4_32 = p32[12];
        saved_cs32   = p32[13];

        /* --- raw capture debug (delete once the round trip works) --- */
        serial_puts("efi64boot: raw cap[24..37] ");
        for (int b = 24; b < 38; b++) {
            static char hx[4];
            unsigned char v = pr[b];
            hx[0] = "0123456789ABCDEF"[v >> 4];
            hx[1] = "0123456789ABCDEF"[v & 0xF];
            hx[2] = ' ';
            hx[3] = 0;
            serial_puts(hx);
        }
        serial_puts("\n");
        serial_puts("efi64boot: saved_gdtr32      : ");
        for (int b = 0; b < 10; b++) {
            static char hx[4];
            unsigned char v = ((unsigned char *)saved_gdtr32)[b];
            hx[0] = "0123456789ABCDEF"[v >> 4];
            hx[1] = "0123456789ABCDEF"[v & 0xF];
            hx[2] = ' ';
            hx[3] = 0;
            serial_puts(hx);
        }
        serial_puts("\n");
        serial_puts("efi64boot: cap cr0=0x");
        serial_hex(p32[10]);
        serial_puts(" cr3=0x");
        serial_hex(p32[11]);
        serial_puts(" cr4=0x");
        serial_hex(p32[12]);
        serial_puts(" cs=0x");
        serial_hex(p32[13]);
        serial_puts("\n");
        saved_ds32   = p32[14];
        saved_es32   = p32[15];
        saved_ss32   = p32[16];
        saved_fs32   = p32[17];
        saved_gs32   = p32[18];
        thunk_stack32_top = p32[19] - 0x400; /* below firmware ESP */

        serial_puts("efi64boot: fw GDT limit=0x");
        serial_hex(pr[24] | (pr[25] << 8));
        serial_puts(" base=0x");
        serial_hex(pr[26] | (pr[27] << 8) | (pr[28] << 16) | (pr[29] << 24));
        serial_puts(" cs=0x");
        serial_hex(p32[13]);
        serial_puts(" ds=0x");
        serial_hex(p32[14]);
        serial_puts("\n");

        fake64_init((void *)(uintptr_t)p32[20]); /* bsv32 */
        fake64_bind_services();
        serial_puts("efi64boot: svc table bound, calling Stall(1000000) via thunk...\n");

        {
            uint64_t args[8] = { 1000000, 0, 0, 0, 0, 0, 0, 0 };
            uint64_t ret = 0xdeadbeefdeadbeefULL;
            do_thunk_call(args, &ret, SVC_STALL, 1);
            serial_puts("efi64boot: thunk returned status=0x");
            serial_hex(ret);
            serial_puts(" (0=success = ROUND TRIP COMPLETE)\n");
        }

        /* second round trip: AllocatePool(512) */
        {
            uint64_t args[8] = { 2 /*EfiBootServicesData*/, 512,
                                 (uint64_t)(uintptr_t)&args[4], 0,0,0,0,0 };
            uint64_t ret = 0;
            do_thunk_call(args, &ret, 5 /*SVC_ALLOCATEPOOL*/, 3);
            serial_puts("efi64boot: AllocatePool status=0x");
            serial_hex(ret);
            serial_puts(" pool=0x");
            serial_hex(args[4]);
            serial_puts("\n");
        }
    }

    for (;;)
        __asm__ volatile("hlt");
}
