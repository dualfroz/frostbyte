#include "klib.h"
#include "string.h"

/* ---- character classification -------------------------------------- */

bool isDigit(int c) { return c >= '0' && c <= '9'; }
bool isUpper(int c) { return c >= 'A' && c <= 'Z'; }
bool isLower(int c) { return c >= 'a' && c <= 'z'; }
bool isAlpha(int c) { return isUpper(c) || isLower(c); }
bool isAlnum(int c) { return isAlpha(c) || isDigit(c); }
bool isSpace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
bool isHexDigit(int c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

int toUpper(int c) { return isLower(c) ? c - 'a' + 'A' : c; }
int toLower(int c) { return isUpper(c) ? c - 'A' + 'a' : c; }

/* ---- string manipulation --------------------------------------------- */

char* strcpy(char* dest, const char* src)
{
    char* out = dest;
    while ((*out++ = *src++) != 0)
        ;
    return dest;
}

char* strncpy(char* dest, const char* src, usize count)
{
    usize i = 0;
    for (; i < count && src[i]; i++)
        dest[i] = src[i];
    for (; i < count; i++)
        dest[i] = 0;
    return dest;
}

char* strcat(char* dest, const char* src)
{
    strcpy(dest + strlen(dest), src);
    return dest;
}

char* strncat(char* dest, const char* src, usize count)
{
    char* end = dest + strlen(dest);
    usize i = 0;
    for (; i < count && src[i]; i++)
        end[i] = src[i];
    end[i] = 0;
    return dest;
}

char* strchr(const char* str, int c)
{
    for (;; str++) {
        if (*str == (char)c)
            return (char*)str;
        if (*str == 0)
            return nullptr;
    }
}

char* strrchr(const char* str, int c)
{
    const char* last = nullptr;
    for (;; str++) {
        if (*str == (char)c)
            last = str;
        if (*str == 0)
            return (char*)last;
    }
}

char* strstr(const char* haystack, const char* needle)
{
    if (!*needle)
        return (char*)haystack;
    usize needleLen = strlen(needle);
    for (; *haystack; haystack++) {
        if (memcmp(haystack, needle, needleLen) == 0)
            return (char*)haystack;
    }
    return nullptr;
}

usize strspn(const char* str, const char* accept)
{
    usize count = 0;
    for (; str[count]; count++) {
        if (!strchr(accept, str[count]))
            break;
    }
    return count;
}

usize strcspn(const char* str, const char* reject)
{
    usize count = 0;
    for (; str[count]; count++) {
        if (strchr(reject, str[count]))
            break;
    }
    return count;
}

char* strTokR(char* str, const char* delim, char** savePtr)
{
    char* cursor = str ? str : *savePtr;
    if (cursor == nullptr)
        return nullptr;

    cursor += strspn(cursor, delim);
    if (*cursor == 0) {
        *savePtr = nullptr;
        return nullptr;
    }

    char* tokenEnd = cursor + strcspn(cursor, delim);
    if (*tokenEnd) {
        *tokenEnd = 0;
        *savePtr = tokenEnd + 1;
    } else {
        *savePtr = nullptr;
    }
    return cursor;
}

/* ---- number parsing ---------------------------------------------------- */

static int digitValue(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return -1;
}

uint64 strtoul(const char* str, char** endPtr, int base)
{
    const char* cursor = str;
    while (isSpace((unsigned char)*cursor))
        cursor++;

    bool negative = false;
    if (*cursor == '+' || *cursor == '-') {
        negative = (*cursor == '-');
        cursor++;
    }

    if ((base == 0 || base == 16) && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
        cursor += 2;
        base = 16;
    } else if (base == 0 && cursor[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }

    uint64 value = 0;
    const char* firstDigit = cursor;
    for (;; cursor++) {
        int digit = digitValue((unsigned char)*cursor);
        if (digit < 0 || digit >= base)
            break;
        value = value * (uint64)base + (uint64)digit;
    }

    if (endPtr)
        *endPtr = (char*)(cursor == firstDigit ? str : cursor);
    return negative ? (uint64)(-(int64)value) : value;
}

int64 strtol(const char* str, char** endPtr, int base)
{
    /* strtoul already skips whitespace, consumes an optional sign and
       applies it (via unsigned two's-complement negation) before we ever
       see the result, so reinterpreting its bits as signed is correct
       on its own -- negating again here would flip the sign twice. */
    return (int64)strtoul(str, endPtr, base);
}

int32 atoi(const char* str) { return (int32)strtol(str, nullptr, 10); }
int64 atol(const char* str) { return strtol(str, nullptr, 10); }

/* ---- formatted output --------------------------------------------------- */

typedef struct SnprintfSink {
    char* buf;
    usize size;   /* capacity including the room for a nul terminator */
    usize written; /* total chars that would be written, unbounded */
} SnprintfSink;

static void sinkPutChar(SnprintfSink* sink, char c)
{
    if (sink->written + 1 < sink->size)
        sink->buf[sink->written] = c;
    sink->written++;
}

static void sinkPutString(SnprintfSink* sink, const char* str)
{
    while (*str)
        sinkPutChar(sink, *str++);
}

static void sinkPutUnsigned(SnprintfSink* sink, uint64 value, uint32 base, bool upper, int minWidth, char pad)
{
    static const char* lower = "0123456789abcdef";
    static const char* higher = "0123456789ABCDEF";
    const char* digits = upper ? higher : lower;
    char buffer[32];
    int length = 0;

    if (value == 0)
        buffer[length++] = '0';
    while (value > 0) {
        buffer[length++] = digits[value % base];
        value /= base;
    }
    while (length < minWidth && length < (int)sizeof(buffer))
        buffer[length++] = pad;

    while (length > 0)
        sinkPutChar(sink, buffer[--length]);
}

static void sinkPutSigned(SnprintfSink* sink, int64 value, int minWidth, char pad)
{
    if (value < 0) {
        sinkPutChar(sink, '-');
        sinkPutUnsigned(sink, (uint64)(-value), 10, false, minWidth > 0 ? minWidth - 1 : 0, pad);
    } else {
        sinkPutUnsigned(sink, (uint64)value, 10, false, minWidth, pad);
    }
}

int vsnprintf(char* buf, usize size, const char* format, va_list args)
{
    SnprintfSink sink = { buf, size, 0 };

    for (usize i = 0; format[i]; i++) {
        if (format[i] != '%') {
            sinkPutChar(&sink, format[i]);
            continue;
        }

        i++;
        int width = 0;
        char pad = ' ';
        if (format[i] == '0') {
            pad = '0';
            i++;
        }
        while (format[i] >= '0' && format[i] <= '9') {
            width = width * 10 + (format[i] - '0');
            i++;
        }

        int longCount = 0;
        while (format[i] == 'l') {
            longCount++;
            i++;
        }

        switch (format[i]) {
        case 'c':
            sinkPutChar(&sink, (char)va_arg(args, int));
            break;
        case 's':
            sinkPutString(&sink, va_arg(args, const char*));
            break;
        case 'd':
        case 'i':
            sinkPutSigned(&sink, longCount ? va_arg(args, int64) : (int64)va_arg(args, int), width, pad);
            break;
        case 'u':
            sinkPutUnsigned(&sink, longCount ? va_arg(args, uint64) : (uint64)va_arg(args, uint32), 10, false, width, pad);
            break;
        case 'x':
            sinkPutUnsigned(&sink, longCount ? va_arg(args, uint64) : (uint64)va_arg(args, uint32), 16, false, width, pad);
            break;
        case 'X':
            sinkPutUnsigned(&sink, longCount ? va_arg(args, uint64) : (uint64)va_arg(args, uint32), 16, true, width, pad);
            break;
        case 'p':
            sinkPutString(&sink, "0x");
            sinkPutUnsigned(&sink, (uint64)va_arg(args, void*), 16, false, 16, '0');
            break;
        case '%':
            sinkPutChar(&sink, '%');
            break;
        default:
            sinkPutChar(&sink, '%');
            if (format[i])
                sinkPutChar(&sink, format[i]);
            break;
        }
    }

    if (size > 0)
        buf[sink.written < size ? sink.written : size - 1] = 0;
    return (int)sink.written;
}

int snprintf(char* buf, usize size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buf, size, format, args);
    va_end(args);
    return result;
}

/* ---- generic sorting / searching ---------------------------------------- */

static void rawSwap(uint8* a, uint8* b, usize size)
{
    for (usize i = 0; i < size; i++) {
        uint8 tmp = a[i];
        a[i] = b[i];
        b[i] = tmp;
    }
}

/* Insertion sort: O(n^2) but allocation-free and simple to prove correct;
   fine for the small, bounded arrays a freestanding kernel actually sorts
   (dirents in a directory, a handful of scheduler entries, ...). */
void qsort(void* base, usize count, usize size, int (*cmp)(const void*, const void*))
{
    uint8* items = (uint8*)base;
    for (usize i = 1; i < count; i++) {
        usize j = i;
        while (j > 0 && cmp(items + j * size, items + (j - 1) * size) < 0) {
            rawSwap(items + j * size, items + (j - 1) * size, size);
            j--;
        }
    }
}

void* bsearch(const void* key, const void* base, usize count, usize size,
              int (*cmp)(const void*, const void*))
{
    const uint8* items = (const uint8*)base;
    usize low = 0;
    usize high = count;

    while (low < high) {
        usize mid = low + (high - low) / 2;
        const uint8* candidate = items + mid * size;
        int result = cmp(key, candidate);
        if (result == 0)
            return (void*)candidate;
        if (result < 0)
            high = mid;
        else
            low = mid + 1;
    }
    return nullptr;
}
