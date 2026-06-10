#ifndef FROSTBYTE_PIC_H
#define FROSTBYTE_PIC_H

#include "types.h"

void picRemap(void);
void picSendEoi(uint8 irq);
void picSetMask(uint8 irq);
void picClearMask(uint8 irq);

#endif
