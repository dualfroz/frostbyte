#ifndef FROSTBYTE_ATA_H
#define FROSTBYTE_ATA_H

#include "types.h"

#define ataSectorSize 512

bool ataInit(void);
bool ataPresent(void);
uint64 ataSectorCount(void);
const char* ataModelString(void);

bool ataReadSectors(uint64 lba, uint32 count, void* buffer);
bool ataWriteSectors(uint64 lba, uint32 count, const void* buffer);

#endif
