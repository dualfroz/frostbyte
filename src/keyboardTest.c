#include "ktest.h"
#include "keyboard.h"
#include "string.h"

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

    keyboardHandleScancode(scancodeK | 0x80);
    char c;
    KT_EQ(t, keyboardReaderRead(h, &c, 1), -1);

    keyboardHandleScancode(0);
    KT_EQ(t, keyboardReaderRead(h, &c, 1), -1);

    keyboardCloseReader(h);
}

KTEST(keyboardReaderReadDeliversQueuedBytesInFifoOrderAcrossPartialReads)
{
    int h = keyboardOpenReader();
    KT_NE(t, h, consoleReaderInvalid);

    keyboardHandleScancode(scancodeK);
    keyboardHandleScancode(scancodeK);
    keyboardHandleScancode(scancodeK);

    char buffer[8];
    memset(buffer, 0, sizeof(buffer));

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

    KT_EQP(t, keyboardReaderChannel(a), channelA);

    keyboardCloseReader(a);
    KT_NULL(t, keyboardReaderChannel(a));

    keyboardCloseReader(b);
}

KTEST(keyboardOpenReaderExhaustionThenCloseFreesSlotForReuse)
{
    int handles[maxConsoleReaders];
    int madeCount = 0;
    while (madeCount < maxConsoleReaders) {
        int h = keyboardOpenReader();
        if (h == consoleReaderInvalid)
            break;
        handles[madeCount++] = h;
    }
    KT_GT(t, madeCount, 0);

    KT_EQ(t, keyboardOpenReader(), consoleReaderInvalid);

    keyboardCloseReader(handles[0]);
    int reused = keyboardOpenReader();
    KT_NE(t, reused, consoleReaderInvalid);
    KT_EQ(t, reused, handles[0]);

    keyboardCloseReader(reused);
    for (int i = 1; i < madeCount; i++)
        keyboardCloseReader(handles[i]);
}
