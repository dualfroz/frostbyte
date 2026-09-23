#include "ata.h"
#include "ports.h"

#define ataRegData      0x1F0
#define ataRegError     0x1F1
#define ataRegSectorCnt 0x1F2
#define ataRegLbaLow    0x1F3
#define ataRegLbaMid    0x1F4
#define ataRegLbaHigh   0x1F5
#define ataRegDriveHead 0x1F6
#define ataRegStatus    0x1F7
#define ataRegCommand   0x1F7
#define ataRegControl   0x3F6

#define ataStatusErr 0x01
#define ataStatusDrq 0x08
#define ataStatusDf  0x20
#define ataStatusBsy 0x80

#define ataCmdReadSectors  0x20
#define ataCmdWriteSectors 0x30
#define ataCmdCacheFlush   0xE7
#define ataCmdIdentify     0xEC

#define ataMaxSectorsPerCommand 255
#define ataPollTimeout 1000000u

static bool diskPresent;
static uint64 totalSectors;
static char modelBuffer[41];

static void ataIoDelay(void)
{
    inb(ataRegControl);
    inb(ataRegControl);
    inb(ataRegControl);
    inb(ataRegControl);
}

static bool ataWaitClear(uint8 mask)
{
    for (uint32 i = 0; i < ataPollTimeout; i++)
        if (!(inb(ataRegStatus) & mask))
            return true;
    return false;
}

static bool ataWaitDrqOrErr(bool* err)
{
    for (uint32 i = 0; i < ataPollTimeout; i++) {
        uint8 status = inb(ataRegStatus);
        if (status & (ataStatusErr | ataStatusDf)) {
            *err = true;
            return true;
        }
        if (status & ataStatusDrq) {
            *err = false;
            return true;
        }
    }
    return false;
}

static void ataSelect(uint64 lba, uint8 count)
{
    outb(ataRegDriveHead, (uint8)(0xE0 | ((lba >> 24) & 0x0F)));
    ataIoDelay();
    outb(ataRegSectorCnt, count);
    outb(ataRegLbaLow, (uint8)(lba & 0xFF));
    outb(ataRegLbaMid, (uint8)((lba >> 8) & 0xFF));
    outb(ataRegLbaHigh, (uint8)((lba >> 16) & 0xFF));
}

bool ataInit(void)
{
    diskPresent = false;
    totalSectors = 0;
    modelBuffer[0] = 0;

    outb(ataRegControl, 0x02);

    outb(ataRegDriveHead, 0xA0);
    ataIoDelay();
    outb(ataRegSectorCnt, 0);
    outb(ataRegLbaLow, 0);
    outb(ataRegLbaMid, 0);
    outb(ataRegLbaHigh, 0);
    outb(ataRegCommand, ataCmdIdentify);

    if (inb(ataRegStatus) == 0)
        return false;

    if (!ataWaitClear(ataStatusBsy))
        return false;

    if (inb(ataRegLbaMid) != 0 || inb(ataRegLbaHigh) != 0)
        return false;

    bool err = false;
    if (!ataWaitDrqOrErr(&err) || err)
        return false;

    uint16 identifyWords[256];
    for (int i = 0; i < 256; i++)
        identifyWords[i] = inw(ataRegData);

    totalSectors = (uint64)identifyWords[60] | ((uint64)identifyWords[61] << 16);

    for (int i = 0; i < 20; i++) {
        uint16 word = identifyWords[27 + i];
        modelBuffer[i * 2] = (char)(word >> 8);
        modelBuffer[i * 2 + 1] = (char)(word & 0xFF);
    }
    modelBuffer[40] = 0;
    for (int i = 39; i >= 0 && modelBuffer[i] == ' '; i--)
        modelBuffer[i] = 0;

    diskPresent = totalSectors > 0;
    return diskPresent;
}

bool ataPresent(void)
{
    return diskPresent;
}

uint64 ataSectorCount(void)
{
    return totalSectors;
}

const char* ataModelString(void)
{
    return modelBuffer;
}

bool ataReadSectors(uint64 lba, uint32 count, void* buffer)
{
    if (count == 0)
        return true;
    if (!diskPresent || count > ataMaxSectorsPerCommand || lba + count > totalSectors)
        return false;

    if (!ataWaitClear(ataStatusBsy))
        return false;
    ataSelect(lba, (uint8)count);
    outb(ataRegCommand, ataCmdReadSectors);

    uint16* dst = (uint16*)buffer;
    for (uint32 s = 0; s < count; s++) {
        if (!ataWaitClear(ataStatusBsy))
            return false;
        bool err = false;
        if (!ataWaitDrqOrErr(&err) || err)
            return false;
        for (int w = 0; w < 256; w++)
            dst[w] = inw(ataRegData);
        dst += 256;
    }
    return true;
}

bool ataWriteSectors(uint64 lba, uint32 count, const void* buffer)
{
    if (count == 0)
        return true;
    if (!diskPresent || count > ataMaxSectorsPerCommand || lba + count > totalSectors)
        return false;

    if (!ataWaitClear(ataStatusBsy))
        return false;
    ataSelect(lba, (uint8)count);
    outb(ataRegCommand, ataCmdWriteSectors);

    const uint16* src = (const uint16*)buffer;
    for (uint32 s = 0; s < count; s++) {
        if (!ataWaitClear(ataStatusBsy))
            return false;
        bool err = false;
        if (!ataWaitDrqOrErr(&err) || err)
            return false;
        for (int w = 0; w < 256; w++)
            outw(ataRegData, src[w]);
        src += 256;
        ataIoDelay();
    }

    outb(ataRegCommand, ataCmdCacheFlush);
    if (!ataWaitClear(ataStatusBsy))
        return false;

    return true;
}
