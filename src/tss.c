#include "tss.h"
#include "gdt.h"
#include "string.h"

typedef struct Tss {
    uint32 reserved0;
    uint64 rsp0;
    uint64 rsp1;
    uint64 rsp2;
    uint64 reserved1;
    uint64 ist1;
    uint64 ist2;
    uint64 ist3;
    uint64 ist4;
    uint64 ist5;
    uint64 ist6;
    uint64 ist7;
    uint64 reserved2;
    uint16 reserved3;
    uint16 iomapBase;
} __attribute__((packed)) Tss;

static Tss tss;

void tssInit(uint64 rsp0)
{
    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = rsp0;
    tss.iomapBase = sizeof(Tss);

    gdtInstallTss((uint64)&tss, sizeof(Tss) - 1);
    __asm__ volatile("ltr %w0" : : "r"((uint16)tssSelector));
}

void tssSetRsp0(uint64 rsp0)
{
    tss.rsp0 = rsp0;
}
