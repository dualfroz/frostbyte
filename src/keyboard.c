#include "keyboard.h"
#include "ports.h"
#include "ring.h"
#include "sched.h"
#include "signal.h"

#define keyboardData 0x60

#define scancodeLeftCtrlMake  0x1D
#define scancodeLeftCtrlBreak 0x9D

static bool ctrlHeld;

static uint32 foregroundPgid;

static const char scancodeLower[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0
};

static const char scancodeUpper[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0
};

static bool shiftHeld;

#define consoleReaderBufferSize 32

typedef struct ConsoleReader {
    RingBuffer ring;
    uint8 backing[consoleReaderBufferSize];
    bool used;
} ConsoleReader;

static ConsoleReader consoleReaders[maxConsoleReaders];

void keyboardInit(void)
{
    shiftHeld = false;
    ctrlHeld = false;
    foregroundPgid = 0;
    for (int i = 0; i < maxConsoleReaders; i++)
        consoleReaders[i].used = false;
}

void keyboardSetForegroundPgid(uint32 pgid)
{
    foregroundPgid = pgid;
}

uint32 keyboardForegroundPgid(void)
{
    return foregroundPgid;
}

static int openReader(void)
{
    for (int i = 0; i < maxConsoleReaders; i++) {
        if (!consoleReaders[i].used) {
            ringInit(&consoleReaders[i].ring, consoleReaders[i].backing, consoleReaderBufferSize);
            consoleReaders[i].used = true;
            return i;
        }
    }
    return consoleReaderInvalid;
}

int keyboardOpenReader(void)
{
    uint64 flags = saveAndDisableInterrupts();
    int handle = openReader();
    restoreInterrupts(flags);
    return handle;
}

static ConsoleReader* readerAt(int handle)
{
    if (handle < 0 || handle >= maxConsoleReaders)
        return nullptr;
    if (!consoleReaders[handle].used)
        return nullptr;
    return &consoleReaders[handle];
}

void keyboardCloseReader(int handle)
{
    ConsoleReader* r = readerAt(handle);
    if (!r)
        return;
    r->used = false;
}

static int64 readReader(int handle, void* buffer, uint64 length)
{
    ConsoleReader* r = readerAt(handle);
    if (!r)
        return -1;
    if (length == 0)
        return 0;
    if (ringIsEmpty(&r->ring))
        return -1;

    usize got = ringRead(&r->ring, (uint8*)buffer, (usize)length);
    return (int64)got;
}

int64 keyboardReaderRead(int handle, void* buffer, uint64 length)
{
    uint64 flags = saveAndDisableInterrupts();
    int64 got = readReader(handle, buffer, length);
    restoreInterrupts(flags);
    return got;
}

int64 keyboardReaderReadBlocking(int handle, void* buffer, uint64 length)
{
    ConsoleReader* r = readerAt(handle);
    if (!r)
        return -1;

    for (;;) {
        schedBlockCurrent((const void*)r);
        int64 result = keyboardReaderRead(handle, buffer, length);
        if (result != -1) {
            schedCancelBlockCurrent();
            return result;
        }
        halt();
    }
}

const void* keyboardReaderChannel(int handle)
{
    return (const void*)readerAt(handle);
}

bool keyboardReaderHasData(int handle)
{
    ConsoleReader* r = readerAt(handle);
    if (!r)
        return false;
    return !ringIsEmpty(&r->ring);
}

static void handleScancode(uint8 scancode)
{
    if (scancode == scancodeLeftCtrlMake) {
        ctrlHeld = true;
        return;
    }
    if (scancode == scancodeLeftCtrlBreak) {
        ctrlHeld = false;
        return;
    }
    if (scancode == 0x2A || scancode == 0x36) {
        shiftHeld = true;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shiftHeld = false;
        return;
    }
    if (scancode & 0x80)
        return;

    char c = shiftHeld ? scancodeUpper[scancode] : scancodeLower[scancode];
    if (!c)
        return;

    if (ctrlHeld && (c == 'c' || c == 'C')) {
        sigRaiseGroup(foregroundPgid, sigInt, siCodeKernel, 0);
        return;
    }
    if (ctrlHeld && (c == 'z' || c == 'Z')) {
        sigRaiseGroup(foregroundPgid, sigTstp, siCodeKernel, 0);
        return;
    }

    for (int i = 0; i < maxConsoleReaders; i++) {
        if (!consoleReaders[i].used)
            continue;
        ringPush(&consoleReaders[i].ring, (uint8)c);
        schedWakeChannel((const void*)&consoleReaders[i]);
    }
    schedWakeChannel(schedPollChannel());
}

void keyboardHandleScancode(uint8 scancode)
{
    uint64 flags = saveAndDisableInterrupts();
    handleScancode(scancode);
    restoreInterrupts(flags);
}

void keyboardHandler(void)
{
    uint8 scancode = inb(keyboardData);
    keyboardHandleScancode(scancode);
}
