#ifndef FROSTBYTE_PIT_H
#define FROSTBYTE_PIT_H

#include "types.h"

void pitInit(uint32 frequency);
void pitTick(void);
uint64 pitTicks(void);
uint32 pitFrequency(void);

#endif
