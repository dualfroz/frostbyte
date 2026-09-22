#include "console.h"
#include "vga.h"
#include "serial.h"
#include "ports.h"
#include <stdarg.h>

void consoleInit(void)
{
    serialInit();
    vgaInit();
}

void kputchar(char c)
{
    serialWrite(c);
    vgaPutChar(c);
}

static void writeString(const char* str)
{
    while (*str)
        kputchar(*str++);
}

static void writeUnsigned(uint64 value, uint32 base, bool upper, int minWidth, char pad)
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
    while (length < minWidth)
        buffer[length++] = pad;

    while (length > 0)
        kputchar(buffer[--length]);
}

static void writeSigned(int64 value, int minWidth, char pad)
{
    if (value < 0) {
        kputchar('-');
        writeUnsigned((uint64)(-value), 10, false, minWidth > 0 ? minWidth - 1 : 0, pad);
    } else {
        writeUnsigned((uint64)value, 10, false, minWidth, pad);
    }
}

void kprintf(const char* format, ...)
{
    uint64 flags = saveAndDisableInterrupts();

    va_list args;
    va_start(args, format);

    for (usize i = 0; format[i]; i++) {
        if (format[i] != '%') {
            kputchar(format[i]);
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
            kputchar((char)va_arg(args, int));
            break;
        case 's':
            writeString(va_arg(args, const char*));
            break;
        case 'd':
        case 'i':
            writeSigned(longCount ? va_arg(args, int64) : (int64)va_arg(args, int), width, pad);
            break;
        case 'u':
            writeUnsigned(longCount ? va_arg(args, uint64) : (uint64)va_arg(args, uint32), 10, false, width, pad);
            break;
        case 'x':
            writeUnsigned(longCount ? va_arg(args, uint64) : (uint64)va_arg(args, uint32), 16, false, width, pad);
            break;
        case 'X':
            writeUnsigned(longCount ? va_arg(args, uint64) : (uint64)va_arg(args, uint32), 16, true, width, pad);
            break;
        case 'p':
            writeString("0x");
            writeUnsigned((uint64)va_arg(args, void*), 16, false, 16, '0');
            break;
        case '%':
            kputchar('%');
            break;
        default:
            kputchar('%');
            kputchar(format[i]);
            break;
        }
    }

    va_end(args);
    restoreInterrupts(flags);
}
