#include "vga.h"
#include "ports.h"

#define vgaWidth 80
#define vgaHeight 25
#define vgaBuffer 0xB8000

static volatile uint16* const screen = (volatile uint16*)vgaBuffer;
static uint32 cursorRow;
static uint32 cursorColumn;
static uint8 currentColor;

static uint16 makeCell(char c, uint8 color)
{
    return (uint16)c | ((uint16)color << 8);
}

static void updateHardwareCursor(void)
{
    uint16 position = (uint16)(cursorRow * vgaWidth + cursorColumn);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8)(position & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8)((position >> 8) & 0xFF));
}

void vgaSetColor(VgaColor foreground, VgaColor background)
{
    currentColor = (uint8)foreground | (uint8)(background << 4);
}

void vgaClear(void)
{
    for (uint32 y = 0; y < vgaHeight; y++) {
        for (uint32 x = 0; x < vgaWidth; x++)
            screen[y * vgaWidth + x] = makeCell(' ', currentColor);
    }
    cursorRow = 0;
    cursorColumn = 0;
    updateHardwareCursor();
}

void vgaInit(void)
{
    currentColor = (uint8)vgaLightGrey | (uint8)(vgaBlack << 4);
    vgaClear();
}

static void scroll(void)
{
    for (uint32 y = 1; y < vgaHeight; y++) {
        for (uint32 x = 0; x < vgaWidth; x++)
            screen[(y - 1) * vgaWidth + x] = screen[y * vgaWidth + x];
    }
    for (uint32 x = 0; x < vgaWidth; x++)
        screen[(vgaHeight - 1) * vgaWidth + x] = makeCell(' ', currentColor);
    cursorRow = vgaHeight - 1;
}

void vgaPutChar(char c)
{
    if (c == '\n') {
        cursorColumn = 0;
        cursorRow++;
    } else if (c == '\r') {
        cursorColumn = 0;
    } else if (c == '\b') {
        if (cursorColumn > 0) {
            cursorColumn--;
            screen[cursorRow * vgaWidth + cursorColumn] = makeCell(' ', currentColor);
        }
    } else if (c == '\t') {
        cursorColumn = (cursorColumn + 4) & ~3u;
    } else {
        screen[cursorRow * vgaWidth + cursorColumn] = makeCell(c, currentColor);
        cursorColumn++;
    }

    if (cursorColumn >= vgaWidth) {
        cursorColumn = 0;
        cursorRow++;
    }
    if (cursorRow >= vgaHeight)
        scroll();

    updateHardwareCursor();
}

void vgaWriteString(const char* str)
{
    while (*str)
        vgaPutChar(*str++);
}
