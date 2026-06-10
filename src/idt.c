#include "idt.h"
#include "types.h"

typedef struct IdtEntry {
    uint16 offsetLow;
    uint16 selector;
    uint8 ist;
    uint8 typeAttr;
    uint16 offsetMid;
    uint32 offsetHigh;
    uint32 reserved;
} __attribute__((packed)) IdtEntry;

typedef struct IdtPointer {
    uint16 limit;
    uint64 base;
} __attribute__((packed)) IdtPointer;

static IdtEntry idtEntries[256];
static IdtPointer idtPointer;

extern void* isrStubTable[];
extern void isrStub128(void);

static void setGate(int vector, void* handler, uint8 typeAttr)
{
    uint64 address = (uint64)handler;
    idtEntries[vector].offsetLow = (uint16)(address & 0xFFFF);
    idtEntries[vector].selector = 0x08;
    idtEntries[vector].ist = 0;
    idtEntries[vector].typeAttr = typeAttr;
    idtEntries[vector].offsetMid = (uint16)((address >> 16) & 0xFFFF);
    idtEntries[vector].offsetHigh = (uint32)((address >> 32) & 0xFFFFFFFF);
    idtEntries[vector].reserved = 0;
}

void idtInit(void)
{
    for (int vector = 0; vector < 48; vector++)
        setGate(vector, isrStubTable[vector], 0x8E);

    setGate(0x80, (void*)isrStub128, 0xEE);

    idtPointer.limit = (uint16)(sizeof(idtEntries) - 1);
    idtPointer.base = (uint64)&idtEntries;

    __asm__ volatile("lidt %0" : : "m"(idtPointer));
}
