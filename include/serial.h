#ifndef FROSTBYTE_SERIAL_H
#define FROSTBYTE_SERIAL_H

#include "types.h"

void serialInit(void);
void serialWrite(char c);
void serialWriteString(const char* str);

#endif
