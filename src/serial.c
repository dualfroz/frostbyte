#include "serial.h"
#include "ports.h"

#define comPort1 0x3F8

static bool transmitEmpty(void)
{
    return (inb(comPort1 + 5) & 0x20) != 0;
}

void serialInit(void)
{
    outb(comPort1 + 1, 0x00);
    outb(comPort1 + 3, 0x80);
    outb(comPort1 + 0, 0x01);
    outb(comPort1 + 1, 0x00);
    outb(comPort1 + 3, 0x03);
    outb(comPort1 + 2, 0xC7);
    outb(comPort1 + 4, 0x0B);
}

void serialWrite(char c)
{
    if (c == '\n') {
        while (!transmitEmpty()) { }
        outb(comPort1, '\r');
    }
    while (!transmitEmpty()) { }
    outb(comPort1, (uint8)c);
}

void serialWriteString(const char* str)
{
    while (*str)
        serialWrite(*str++);
}
