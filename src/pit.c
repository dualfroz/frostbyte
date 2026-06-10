#include "pit.h"
#include "ports.h"

#define pitChannel0 0x40
#define pitCommand 0x43
#define pitBaseFrequency 1193182

static volatile uint64 tickCount;
static uint32 configuredFrequency;

void pitInit(uint32 frequency)
{
    configuredFrequency = frequency;
    uint32 divisor = pitBaseFrequency / frequency;

    outb(pitCommand, 0x36);
    outb(pitChannel0, (uint8)(divisor & 0xFF));
    outb(pitChannel0, (uint8)((divisor >> 8) & 0xFF));
}

void pitTick(void)
{
    tickCount++;
}

uint64 pitTicks(void)
{
    return tickCount;
}

uint32 pitFrequency(void)
{
    return configuredFrequency;
}
