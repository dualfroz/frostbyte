#include "keyboard.h"
#include "ports.h"

#define keyboardData 0x60

static const char scancodeLower[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0
};

static const char scancodeUpper[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0
};

#define keyBufferSize 128
static volatile char keyBuffer[keyBufferSize];
static volatile uint32 bufferHead;
static volatile uint32 bufferTail;
static bool shiftHeld;

void keyboardInit(void)
{
    bufferHead = 0;
    bufferTail = 0;
    shiftHeld = false;
}

static void pushChar(char c)
{
    uint32 next = (bufferHead + 1) % keyBufferSize;
    if (next != bufferTail) {
        keyBuffer[bufferHead] = c;
        bufferHead = next;
    }
}

void keyboardHandler(void)
{
    uint8 scancode = inb(keyboardData);

    if (scancode == 0x2A || scancode == 0x36) {
        shiftHeld = true;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shiftHeld = false;
        return;
    }
    if (scancode & 0x80)
        return;

    char c = shiftHeld ? scancodeUpper[scancode] : scancodeLower[scancode];
    if (c)
        pushChar(c);
}

bool keyboardHasChar(void)
{
    return bufferHead != bufferTail;
}

char keyboardReadChar(void)
{
    while (bufferHead == bufferTail)
        __asm__ volatile("sti; hlt");

    char c = keyBuffer[bufferTail];
    bufferTail = (bufferTail + 1) % keyBufferSize;
    return c;
}
