#ifndef FROSTBYTE_CONSOLE_H
#define FROSTBYTE_CONSOLE_H

#include "types.h"

void consoleInit(void);
void kputchar(char c);
void kprintf(const char* format, ...);

#endif
