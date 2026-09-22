#ifndef FROSTBYTE_PIDFD_H
#define FROSTBYTE_PIDFD_H

#include "types.h"

#define maxPidfds 32

int pidfdOpen(uint32 targetId);

uint32 pidfdTarget(int handle);

void pidfdClose(int handle);

#endif
