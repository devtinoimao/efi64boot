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

/* Entry: the trampoline calls here with RDI = PAYLOAD_PARAMS* (our own
 * convention, not any OS ABI — we control both sides). */
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

    for (;;)
        __asm__ volatile("hlt");
}
