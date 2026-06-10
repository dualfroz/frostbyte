#ifndef FROSTBYTE_STRING_H
#define FROSTBYTE_STRING_H

#include "types.h"

void* memset(void* dest, int value, usize count);
void* memcpy(void* dest, const void* src, usize count);
void* memmove(void* dest, const void* src, usize count);
int memcmp(const void* a, const void* b, usize count);
usize strlen(const char* str);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, usize count);

#endif
