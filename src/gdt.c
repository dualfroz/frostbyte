#include "gdt.h"
#include "types.h"

typedef struct GdtPointer {
    uint16 limit;
    uint64 base;
} __attribute__((packed)) GdtPointer;

static uint64 gdtEntries[7];
static GdtPointer gdtPointer;

void gdtInit(void)
{
    gdtEntries[0] = 0x0000000000000000ull;
    gdtEntries[1] = 0x00AF9A000000FFFFull;
    gdtEntries[2] = 0x00CF92000000FFFFull;
    gdtEntries[3] = 0x00AFFA000000FFFFull;
    gdtEntries[4] = 0x00CFF2000000FFFFull;
    gdtEntries[5] = 0x0000000000000000ull;
    gdtEntries[6] = 0x0000000000000000ull;

    gdtPointer.limit = (uint16)(sizeof(gdtEntries) - 1);
    gdtPointer.base = (uint64)&gdtEntries;

    __asm__ volatile(
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        :
        : "m"(gdtPointer)
        : "rax", "memory");
}

void gdtInstallTss(uint64 base, uint32 limit)
{
    uint64 low = 0;
    low |= (uint64)(limit & 0xFFFF);
    low |= (base & 0xFFFFull) << 16;
    low |= ((base >> 16) & 0xFFull) << 32;
    low |= (uint64)0x89 << 40;
    low |= (uint64)((limit >> 16) & 0xF) << 48;
    low |= ((base >> 24) & 0xFFull) << 56;

    gdtEntries[5] = low;
    gdtEntries[6] = (base >> 32) & 0xFFFFFFFFull;
}
