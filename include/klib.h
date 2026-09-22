#ifndef FROSTBYTE_KLIB_H
#define FROSTBYTE_KLIB_H

#include "types.h"
#include <stdarg.h>

bool isDigit(int c);
bool isAlpha(int c);
bool isAlnum(int c);
bool isSpace(int c);
bool isUpper(int c);
bool isLower(int c);
bool isHexDigit(int c);
int toUpper(int c);
int toLower(int c);

char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, usize count);
char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, usize count);
char* strchr(const char* str, int c);
char* strrchr(const char* str, int c);
char* strstr(const char* haystack, const char* needle);
usize strspn(const char* str, const char* accept);
usize strcspn(const char* str, const char* reject);

char* strTokR(char* str, const char* delim, char** savePtr);

int32 atoi(const char* str);
int64 atol(const char* str);

int64 strtol(const char* str, char** endPtr, int base);
uint64 strtoul(const char* str, char** endPtr, int base);

int snprintf(char* buf, usize size, const char* format, ...);
int vsnprintf(char* buf, usize size, const char* format, va_list args);

void qsort(void* base, usize count, usize size, int (*cmp)(const void*, const void*));
void* bsearch(const void* key, const void* base, usize count, usize size,
              int (*cmp)(const void*, const void*));

#endif
