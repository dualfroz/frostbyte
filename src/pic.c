#include "pic.h"
#include "ports.h"

#define picMasterCommand 0x20
#define picMasterData 0x21
#define picSlaveCommand 0xA0
#define picSlaveData 0xA1
#define picEoi 0x20

#define icw1Init 0x11
#define icw4Mode 0x01

#define masterVectorBase 0x20
#define slaveVectorBase 0x28

void picRemap(void)
{
    uint8 masterMask = inb(picMasterData);
    uint8 slaveMask = inb(picSlaveData);

    outb(picMasterCommand, icw1Init);
    ioWait();
    outb(picSlaveCommand, icw1Init);
    ioWait();
    outb(picMasterData, masterVectorBase);
    ioWait();
    outb(picSlaveData, slaveVectorBase);
    ioWait();
    outb(picMasterData, 0x04);
    ioWait();
    outb(picSlaveData, 0x02);
    ioWait();
    outb(picMasterData, icw4Mode);
    ioWait();
    outb(picSlaveData, icw4Mode);
    ioWait();

    outb(picMasterData, masterMask);
    outb(picSlaveData, slaveMask);
}

void picSendEoi(uint8 irq)
{
    if (irq >= 8)
        outb(picSlaveCommand, picEoi);
    outb(picMasterCommand, picEoi);
}

void picSetMask(uint8 irq)
{
    uint16 port = irq < 8 ? picMasterData : picSlaveData;
    uint8 bit = irq < 8 ? irq : (uint8)(irq - 8);
    uint8 value = inb(port) | (uint8)(1 << bit);
    outb(port, value);
}

void picClearMask(uint8 irq)
{
    uint16 port = irq < 8 ? picMasterData : picSlaveData;
    uint8 bit = irq < 8 ? irq : (uint8)(irq - 8);
    uint8 value = inb(port) & (uint8)~(1 << bit);
    outb(port, value);
}
