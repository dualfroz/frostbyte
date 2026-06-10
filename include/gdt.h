#ifndef FROSTBYTE_GDT_H
#define FROSTBYTE_GDT_H

#include "types.h"

#define kernelCodeSelector 0x08
#define kernelDataSelector 0x10
#define userCodeSelector   0x18
#define userDataSelector   0x20
#define tssSelector        0x28

void gdtInit(void);
void gdtInstallTss(uint64 base, uint32 limit);

#endif
