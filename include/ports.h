#ifndef FROSTBYTE_PORTS_H
#define FROSTBYTE_PORTS_H

#include "types.h"

static inline void outb(uint16 port, uint8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8 inb(uint16 port)
{
    uint8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16 port, uint16 value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16 inw(uint16 port)
{
    uint16 value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void ioWait(void)
{
    outb(0x80, 0);
}

static inline uint64 readFlags(void)
{
    uint64 flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return flags;
}

static inline void enableInterrupts(void)
{
    __asm__ volatile("sti");
}

static inline uint64 saveAndDisableInterrupts(void)
{
    uint64 flags = readFlags();
    __asm__ volatile("cli");
    return flags;
}

static inline void restoreInterrupts(uint64 flags)
{
    if (flags & (1u << 9))
        __asm__ volatile("sti");
}

static inline void halt(void)
{
    __asm__ volatile("hlt");
}

#endif
