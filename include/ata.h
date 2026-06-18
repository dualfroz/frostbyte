#ifndef FROSTBYTE_ATA_H
#define FROSTBYTE_ATA_H

#include "types.h"

/*
 * ATA PIO driver (primary bus, master drive only, LBA28 addressing).
 *
 * This talks to real IDE hardware registers (0x1F0-0x1F7 + the 0x3F6
 * alternate-status/device-control port) using polling, not IRQ14: ataInit
 * disables the drive's interrupt line (nIEN) up front, so every command is
 * driven by busy-waiting on the status register. That keeps the driver
 * usable from the in-kernel test harness, which runs before the scheduler
 * and with interrupts disabled.
 *
 * All polling loops are bounded (ataPollTimeout iterations): a missing or
 * wedged drive returns false/failure instead of hanging the kernel forever.
 */

#define ataSectorSize 512

bool ataInit(void);
bool ataPresent(void);
uint64 ataSectorCount(void);
const char* ataModelString(void);

/* count is a sector count in [0, 255]; 0 trivially succeeds without
   touching hardware. Both fail (return false) if no drive was found by
   ataInit, if the request runs past ataSectorCount(), or if the drive
   reports an error/timeout mid-transfer. */
bool ataReadSectors(uint64 lba, uint32 count, void* buffer);
bool ataWriteSectors(uint64 lba, uint32 count, const void* buffer);

#endif
