#ifndef FROSTBYTE_KEYBOARD_H
#define FROSTBYTE_KEYBOARD_H

#include "types.h"

void keyboardInit(void);

void keyboardHandler(void);

#define maxConsoleReaders 296
#define consoleReaderInvalid (-1)

int keyboardOpenReader(void);

void keyboardCloseReader(int handle);

int64 keyboardReaderRead(int handle, void* buffer, uint64 length);

int64 keyboardReaderReadBlocking(int handle, void* buffer, uint64 length);

const void* keyboardReaderChannel(int handle);

bool keyboardReaderHasData(int handle);

void keyboardHandleScancode(uint8 scancode);

void keyboardSetForegroundPgid(uint32 pgid);

uint32 keyboardForegroundPgid(void);

#endif
