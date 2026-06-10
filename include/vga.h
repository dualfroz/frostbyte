#ifndef FROSTBYTE_VGA_H
#define FROSTBYTE_VGA_H

#include "types.h"

typedef enum {
    vgaBlack = 0,
    vgaBlue = 1,
    vgaGreen = 2,
    vgaCyan = 3,
    vgaRed = 4,
    vgaMagenta = 5,
    vgaBrown = 6,
    vgaLightGrey = 7,
    vgaDarkGrey = 8,
    vgaLightBlue = 9,
    vgaLightGreen = 10,
    vgaLightCyan = 11,
    vgaLightRed = 12,
    vgaPink = 13,
    vgaYellow = 14,
    vgaWhite = 15
} VgaColor;

void vgaInit(void);
void vgaClear(void);
void vgaPutChar(char c);
void vgaWriteString(const char* str);
void vgaSetColor(VgaColor foreground, VgaColor background);

#endif
