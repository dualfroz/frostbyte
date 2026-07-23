#include "ktest.h"
#include "keyboard.h"
#include "string.h"

/*
 * Coverage for keyboard.c's per-reader console input queues (keyboard.h) --
 * the fan-out/blocking-channel machinery added this wave to close the gap
 * called out at the end of the previous one: a single global keyboard
 * buffer shared by every consumer, with no per-process queueing and no
 * scheduler-visible blocking. What is pure state machine (open/close,
 * scancode decoding, fan-out to N independent queues, exhaustion/reuse)
 * lives here, driven directly through keyboardHandleScancode -- the same
 * seam the live PS/2 IRQ handler (keyboardHandler) calls into, just fed a
 * synthetic scancode instead of a real inb() -- exactly the way
 * pipeShareTest.c drives pipe.c's state machine without a live scheduler.
 * The genuine cross-process park/wake handshake (real blocking read() on
 * fd 0 from two independent ring-3 processes, woken by a real keystroke)
 * only runs for real at boot (kmain.c's consolereader1/consolereader2 demo,
 * needles "consolereader1: got byte" / "consolereader2: got byte" in
 * tools/ci-test.sh), matching how every other blocking-I/O subsystem in
 * this kernel splits its test coverage.
 *
 * Scancode reference (set 1, US QWERTY, keyboard.c's own tables): 0x25 is
 * 'k'/'K', 0x2A/0xAA are left-shift make/break.
 */
#define scancodeK 0x25
#define scancodeLeftShiftMake 0x2A
#define scancodeLeftShiftBreak 0xAA

KTEST(keyboardOpenReaderReturnsDistinctHandlesAndEmptyReadWouldBlock)
{
    int a = keyboardOpenReader();
    int b = keyboardOpenReader();
    KT_NE(t, a, consoleReaderInvalid);
    KT_NE(t, b, consoleReaderInvalid);
    KT_NE(t, a, b);

    char buffer[4];
    KT_EQ(t, keyboardReaderRead(a, buffer, sizeof(buffer)), -1);
    KT_EQ(t, keyboardReaderRead(b, buffer, sizeof(buffer)), -1);

    keyboardCloseReader(a);
    keyboardCloseReader(b);
}

KTEST(keyboardReaderReadRejectsInvalidHandlesAndZeroLength)
{
    KT_EQ(t, keyboardReaderRead(consoleReaderInvalid, nullptr, 4), -1);
    KT_EQ(t, keyboardReaderRead(9999, nullptr, 4), -1);

    int h = keyboardOpenReader();
    KT_NE(t, h, consoleReaderInvalid);
    KT_EQ(t, keyboardReaderRead(h, nullptr, 0), 0);
    keyboardCloseReader(h);

    /* Once closed, the handle is dead -- not silently reusable by the
       caller who just gave it up. */
    KT_EQ(t, keyboardReaderRead(h, nullptr, 0), -1);
}

KTEST(keyboardHandleScancodeBroadcastsToEveryOpenReaderIndependently)
{
    int a = keyboardOpenReader();
    int b = keyboardOpenReader();
    KT_NE(t, a, consoleReaderInvalid);
    KT_NE(t, b, consoleReaderInvalid);

    keyboardHandleScancode(scancodeK);

    char ca = 0, cb = 0;
    KT_EQ(t, keyboardReaderRead(a, &ca, 1), 1);
    KT_EQ(t, keyboardReaderRead(b, &cb, 1), 1);
    KT_EQ(t, ca, 'k');
    KT_EQ(t, cb, 'k');

    /* Each reader's copy is independent: draining a's queue must not touch
       b's, and vice versa -- proven above by both still having their own
       byte to read rather than one starving the other. Both are empty now. */
    KT_EQ(t, keyboardReaderRead(a, &ca, 1), -1);
    KT_EQ(t, keyboardReaderRead(b, &cb, 1), -1);

    keyboardCloseReader(a);
    keyboardCloseReader(b);
}

KTEST(keyboardHandleScancodeAppliesShiftMakeAndBreak)
{
    int h = keyboardOpenReader();
    KT_NE(t, h, consoleReaderInvalid);

    keyboardHandleScancode(scancodeK);
    char c = 0;
    KT_EQ(t, keyboardReaderRead(h, &c, 1), 1);
    KT_EQ(t, c, 'k');

    keyboardHandleScancode(scancodeLeftShiftMake);
    keyboardHandleScancode(scancodeK);
    KT_EQ(t, keyboardReaderRead(h, &c, 1), 1);
    KT_EQ(t, c, 'K');

    keyboardHandleScancode(scancodeLeftShiftBreak);
    keyboardHandleScancode(scancodeK);
    KT_EQ(t, keyboardReaderRead(h, &c, 1), 1);
    KT_EQ(t, c, 'k');

    keyboardCloseReader(h);
}

KTEST(keyboardHandleScancodeIgnoresKeyReleaseAndUnmappedCodes)
{
    int h = keyboardOpenReader();
    KT_NE(t, h, consoleReaderInvalid);

    /* Key-release (high bit set) for 'k' must queue nothing. */
    keyboardHandleScancode(scancodeK | 0x80);
    char c;
    KT_EQ(t, keyboardReaderRead(h, &c, 1), -1);

    /* Scancode 0 maps to 0 (no character) in both tables. */
    keyboardHandleScancode(0);
    KT_EQ(t, keyboardReaderRead(h, &c, 1), -1);

    keyboardCloseReader(h);
}

KTEST(keyboardReaderReadDeliversQueuedBytesInFifoOrderAcrossPartialReads)
{
    int h = keyboardOpenReader();
    KT_NE(t, h, consoleReaderInvalid);

    /* "kkk" via three separate scancode events, like three real keystrokes. */
    keyboardHandleScancode(scancodeK);
    keyboardHandleScancode(scancodeK);
    keyboardHandleScancode(scancodeK);

    char buffer[8];
    memset(buffer, 0, sizeof(buffer));

    /* Ask for only 2 of the 3 queued bytes -- must not block or fail, and
       must leave the third for the next call, same partial-transfer
       contract ring.h's ringRead already documents. */
    int64 got = keyboardReaderRead(h, buffer, 2);
    KT_EQ(t, got, 2);
    KT_EQ(t, buffer[0], 'k');
    KT_EQ(t, buffer[1], 'k');

    got = keyboardReaderRead(h, buffer, 8);
    KT_EQ(t, got, 1);
    KT_EQ(t, buffer[0], 'k');

    KT_EQ(t, keyboardReaderRead(h, buffer, 1), -1);

    keyboardCloseReader(h);
}

KTEST(keyboardReaderChannelIsNullForInvalidAndDistinctPerReader)
{
    KT_NULL(t, keyboardReaderChannel(consoleReaderInvalid));
    KT_NULL(t, keyboardReaderChannel(9999));

    int a = keyboardOpenReader();
    int b = keyboardOpenReader();
    KT_NE(t, a, consoleReaderInvalid);
    KT_NE(t, b, consoleReaderInvalid);

    const void* channelA = keyboardReaderChannel(a);
    const void* channelB = keyboardReaderChannel(b);
    KT_NOTNULL(t, channelA);
    KT_NOTNULL(t, channelB);
    KT_NE(t, (int64)(uint64)channelA, (int64)(uint64)channelB);

    /* Stable across repeated calls -- schedBlockCurrent/schedWakeChannel
       depend on the same reader always producing the same identity. */
    KT_EQP(t, keyboardReaderChannel(a), channelA);

    /* A closed reader's channel must go back to null immediately -- a
       syscall handler using this as its retry condition must never park a
       thread on a channel nobody can ever wake again. */
    keyboardCloseReader(a);
    KT_NULL(t, keyboardReaderChannel(a));

    keyboardCloseReader(b);
}

KTEST(keyboardOpenReaderExhaustionThenCloseFreesSlotForReuse)
{
    /* Does not assume the table starts fully empty -- by the time ktests
       run, schedInit() has already given the idle thread (id 0) its own fd
       0 reader (vfs.c's vfsFdInit), same convention vfsFifoTest.c's own
       pipe-table exhaustion test uses for the shared pipes[] table. */
    int handles[maxConsoleReaders];
    int madeCount = 0;
    while (madeCount < maxConsoleReaders) {
        int h = keyboardOpenReader();
        if (h == consoleReaderInvalid)
            break;
        handles[madeCount++] = h;
    }
    KT_GT(t, madeCount, 0);

    /* Table is now genuinely full: one more must fail cleanly. */
    KT_EQ(t, keyboardOpenReader(), consoleReaderInvalid);

    keyboardCloseReader(handles[0]);
    int reused = keyboardOpenReader();
    KT_NE(t, reused, consoleReaderInvalid);
    KT_EQ(t, reused, handles[0]);

    /* Leave the shared reader table exactly as free as it was found, so
       later boot-time console users (the shell, and the
       consolereader1/consolereader2 live demo) are never starved by this
       test having run. */
    keyboardCloseReader(reused);
    for (int i = 1; i < madeCount; i++)
        keyboardCloseReader(handles[i]);
}
