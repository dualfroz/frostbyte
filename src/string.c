#include "string.h"

void* memset(void* dest, int value, usize count)
{
    uint8* target = (uint8*)dest;
    for (usize i = 0; i < count; i++)
        target[i] = (uint8)value;
    return dest;
}

void* memcpy(void* dest, const void* src, usize count)
{
    uint8* target = (uint8*)dest;
    const uint8* source = (const uint8*)src;
    for (usize i = 0; i < count; i++)
        target[i] = source[i];
    return dest;
}

void* memmove(void* dest, const void* src, usize count)
{
    uint8* target = (uint8*)dest;
    const uint8* source = (const uint8*)src;
    if (target < source) {
        for (usize i = 0; i < count; i++)
            target[i] = source[i];
    } else {
        for (usize i = count; i > 0; i--)
            target[i - 1] = source[i - 1];
    }
    return dest;
}

int memcmp(const void* a, const void* b, usize count)
{
    const uint8* left = (const uint8*)a;
    const uint8* right = (const uint8*)b;
    for (usize i = 0; i < count; i++) {
        if (left[i] != right[i])
            return (int)left[i] - (int)right[i];
    }
    return 0;
}

usize strlen(const char* str)
{
    usize length = 0;
    while (str[length])
        length++;
    return length;
}

int strcmp(const char* a, const char* b)
{
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)(uint8)*a - (int)(uint8)*b;
}

int strncmp(const char* a, const char* b, usize count)
{
    for (usize i = 0; i < count; i++) {
        if (a[i] != b[i] || a[i] == 0)
            return (int)(uint8)a[i] - (int)(uint8)b[i];
    }
    return 0;
}
