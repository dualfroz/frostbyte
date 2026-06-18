#ifndef FROSTBYTE_KLIB_H
#define FROSTBYTE_KLIB_H

#include "types.h"
#include <stdarg.h>

/*
 * klib: freestanding string/number utilities that build on top of the
 * primitive mem-family and str-family routines in string.h.  No libc, no
 * heap dependency (callers pass their own buffers), safe to use before the
 * heap is set up.
 */

/* ---- character classification -------------------------------------- */

bool isDigit(int c);
bool isAlpha(int c);
bool isAlnum(int c);
bool isSpace(int c);
bool isUpper(int c);
bool isLower(int c);
bool isHexDigit(int c);
int toUpper(int c);
int toLower(int c);

/* ---- string manipulation --------------------------------------------
   Semantics match the C standard library equivalents (including the
   footguns: strcpy/strcat assume a big-enough destination). */

char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, usize count);
char* strcat(char* dest, const char* src);
char* strncat(char* dest, const char* src, usize count);
char* strchr(const char* str, int c);
char* strrchr(const char* str, int c);
char* strstr(const char* haystack, const char* needle);
usize strspn(const char* str, const char* accept);
usize strcspn(const char* str, const char* reject);

/* Reentrant tokenizer (no hidden static state, safe across threads).
   Pass str on the first call, nullptr on subsequent calls; *savePtr
   carries the position between calls (caller owns the storage). */
char* strTokR(char* str, const char* delim, char** savePtr);

/* ---- number parsing --------------------------------------------------- */

int32 atoi(const char* str);
int64 atol(const char* str);

/* base 0 means auto-detect: "0x"/"0X" prefix -> 16, leading "0" -> 8,
   otherwise 10. base must be 0 or in [2, 36] otherwise. If endPtr is
   non-null, *endPtr is set to the first unconsumed character. */
int64 strtol(const char* str, char** endPtr, int base);
uint64 strtoul(const char* str, char** endPtr, int base);

/* ---- formatted output into a caller-owned buffer ----------------------
   Same contract as C99 snprintf: writes at most size-1 characters plus a
   terminating nul (unless size == 0), and returns the number of characters
   that *would* have been written given an unbounded buffer. Supports
   %c %s %d %i %u %x %X %p %%, an 'l' length modifier for 64-bit args, and
   a zero-padded/space-padded minimum field width. */
int snprintf(char* buf, usize size, const char* format, ...);
int vsnprintf(char* buf, usize size, const char* format, va_list args);

/* ---- generic sorting / searching --------------------------------------
   Same contract as C's qsort/bsearch: cmp returns <0, 0, >0. */
void qsort(void* base, usize count, usize size, int (*cmp)(const void*, const void*));
void* bsearch(const void* key, const void* base, usize count, usize size,
              int (*cmp)(const void*, const void*));

#endif
