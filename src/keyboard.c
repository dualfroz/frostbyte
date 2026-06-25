#include "keyboard.h"
#include "ports.h"
#include "ring.h"
#include "sched.h"
#include "signal.h"

#define keyboardData 0x60

/* Left-Ctrl's own PS/2 Set 1 make/break scancodes -- exactly the same
   convention keyboardHandleScancode already had for Shift (0x2A/0x36 make,
   0xAA/0xB6 break): held state tracked in a single bool, never forwarded
   to scancodeLower/scancodeUpper (there is no printable character for a
   modifier key on its own). */
#define scancodeLeftCtrlMake  0x1D
#define scancodeLeftCtrlBreak 0x9D

static bool ctrlHeld;

/* Real tcsetpgrp(3)'s target -- see keyboard.h's own doc comment for why 0
   (keyboardInit's initial value) safely means "nobody yet" with no extra
   sentinel needed. */
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

int keyboardOpenReader(void)
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

int64 keyboardReaderRead(int handle, void* buffer, uint64 length)
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

int64 keyboardReaderReadBlocking(int handle, void* buffer, uint64 length)
{
    ConsoleReader* r = readerAt(handle);
    if (!r)
        return -1;

    for (;;) {
        int64 result = keyboardReaderRead(handle, buffer, length);
        if (result != -1)
            return result;

        schedBlockCurrent((const void*)r);
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

/*
 * Real cooked-mode tty line discipline's INTR/SUSP characters (termios's
 * VINTR/VSUSP, both Ctrl-<letter> by default): keyboardHandleScancode now
 * tracks left-Ctrl as a THIRD modifier alongside Shift, and when it is held
 * for a 'c'/'C' or 'z'/'Z' keypress, the resulting sigInt/sigTstp is
 * delivered to the WHOLE foreground process group (sigRaiseGroup, signal.h)
 * -- keyboardSetForegroundPgid's target, real tcsetpgrp(3)'s own job --
 * instead of the raw byte ever reaching any consoleReaders fan-out below.
 * This is the scope real Ctrl-C actually has: every process in the
 * foreground job, not "whichever process happens to be reading fd 0 right
 * now" -- a BACKGROUND job (a different pgid, one keyboardSetForegroundPgid
 * was never told about) must never be disturbed by a keypress aimed at
 * whatever IS currently in the foreground, the same isolation a real shell's
 * job control gives. kmain.c's pgroupfg.c/pgroupbg.c is the live ring-3
 * proof: pgroupfg (foreground, no handler installed) actually dies to the
 * injected Ctrl-C's default-Terminate disposition, while pgroupbg
 * (deliberately left in a DIFFERENT group) proves itself still alive and
 * responsive afterwards.
 */
void keyboardHandleScancode(uint8 scancode)
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

    /* Consumed here, never forwarded as data below -- exactly like a real
       cooked-mode terminal driver never hands a reading program the raw
       0x03/0x1A byte either. siCodeKernel/0: no sending PROCESS raised
       this, the keyboard itself did, the same "kernel, not a process, is
       the source" reasoning schedule()'s own SIGALRM raise already uses
       for siCodeKernel. Harmless no-op (sigRaiseGroup's own contract) if
       foregroundPgid currently names no live group at all. */
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
        /* Mirrors pipeWrite's wake-after-write: harmless no-op (schedWakeChannel's
           own contract) if nobody is currently blocked on this particular
           reader's channel. */
        schedWakeChannel((const void*)&consoleReaders[i]);
    }
    schedWakeChannel(schedPollChannel());
}

void keyboardHandler(void)
{
    uint8 scancode = inb(keyboardData);
    keyboardHandleScancode(scancode);
}
