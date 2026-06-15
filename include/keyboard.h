#ifndef FROSTBYTE_KEYBOARD_H
#define FROSTBYTE_KEYBOARD_H

#include "types.h"

void keyboardInit(void);
void keyboardHandler(void);
bool keyboardHasChar(void);
char keyboardReadChar(void);

#endif
