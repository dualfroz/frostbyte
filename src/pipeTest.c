#include "ktest.h"
#include "pipe.h"
#include "string.h"

KTEST(pipeWriteThenReadRoundTrips)
{
    int h = pipeCreate();
    KT_GE(t, h, 0);

    const char* msg = "hello pipe";
    int64 written = pipeWrite(h, msg, strlen(msg));
    KT_EQ(t, written, (int64)strlen(msg));

    char buffer[32];
    memset(buffer, 0, sizeof(buffer));
    int64 got = pipeRead(h, buffer, sizeof(buffer));
    KT_EQ(t, got, (int64)strlen(msg));
    KT_TRUE(t, memcmp(buffer, msg, strlen(msg)) == 0);

    pipeCloseRead(h);
    pipeCloseWrite(h);
}

KTEST(pipeReadOnEmptyOpenPipeWouldBlock)
{
    int h = pipeCreate();
    KT_GE(t, h, 0);

    char buffer[8];
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), -1);

    pipeCloseRead(h);
    pipeCloseWrite(h);
}

KTEST(pipeReadZeroLengthIsNoop)
{
    int h = pipeCreate();
    KT_GE(t, h, 0);

    char buffer[1];
    KT_EQ(t, pipeRead(h, buffer, 0), 0);
    KT_EQ(t, pipeWrite(h, buffer, 0), 0);

    pipeCloseRead(h);
    pipeCloseWrite(h);
}

KTEST(pipeFullWriteWouldBlockThenDrainsAndAcceptsMore)
{
    int h = pipeCreate();
    KT_GE(t, h, 0);

    uint8 chunk[pipeBufferSize];
    memset(chunk, 0xAB, sizeof(chunk));

    int64 filled = pipeWrite(h, chunk, sizeof(chunk));
    KT_EQ(t, filled, (int64)sizeof(chunk));

    KT_EQ(t, pipeWrite(h, chunk, 1), -1);

    uint8 drained[64];
    int64 gotDrain = pipeRead(h, drained, sizeof(drained));
    KT_EQ(t, gotDrain, (int64)sizeof(drained));
    for (uint64 i = 0; i < sizeof(drained); i++)
        KT_EQU(t, drained[i], 0xAB);

    int64 moreWritten = pipeWrite(h, chunk, sizeof(drained));
    KT_EQ(t, moreWritten, (int64)sizeof(drained));

    pipeCloseRead(h);
    pipeCloseWrite(h);
}

KTEST(pipeEofAfterWriterClosedAndBufferDrained)
{
    int h = pipeCreate();
    KT_GE(t, h, 0);

    const char* msg = "last bytes";
    KT_EQ(t, pipeWrite(h, msg, strlen(msg)), (int64)strlen(msg));

    pipeCloseWrite(h);

    char buffer[32];
    int64 got = pipeRead(h, buffer, sizeof(buffer));
    KT_EQ(t, got, (int64)strlen(msg));

    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), 0);
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), 0);

    pipeCloseRead(h);
}

KTEST(pipeWriteAfterReaderClosedIsBrokenPipe)
{
    int h = pipeCreate();
    KT_GE(t, h, 0);

    pipeCloseRead(h);

    const char* msg = "nobody listening";
    KT_EQ(t, pipeWrite(h, msg, strlen(msg)), -1);

    pipeCloseWrite(h);
}

KTEST(pipeInvalidHandleIsRejected)
{
    KT_FALSE(t, pipeValid(-1));
    KT_FALSE(t, pipeValid(999));

    char buffer[4];
    KT_EQ(t, pipeRead(-1, buffer, sizeof(buffer)), -1);
    KT_EQ(t, pipeWrite(-1, buffer, sizeof(buffer)), -1);

    pipeCloseRead(-1);
    pipeCloseWrite(999);
}

KTEST(pipeIndependentPipesDoNotInterfere)
{
    int a = pipeCreate();
    int b = pipeCreate();
    KT_GE(t, a, 0);
    KT_GE(t, b, 0);
    KT_NE(t, a, b);

    KT_EQ(t, pipeWrite(a, "AAAA", 4), 4);
    KT_EQ(t, pipeWrite(b, "BB", 2), 2);

    char bufA[8];
    char bufB[8];
    memset(bufA, 0, sizeof(bufA));
    memset(bufB, 0, sizeof(bufB));

    KT_EQ(t, pipeRead(b, bufB, sizeof(bufB)), 2);
    KT_TRUE(t, memcmp(bufB, "BB", 2) == 0);

    KT_EQ(t, pipeRead(a, bufA, sizeof(bufA)), 4);
    KT_TRUE(t, memcmp(bufA, "AAAA", 4) == 0);

    pipeCloseRead(a);
    pipeCloseWrite(a);
    pipeCloseRead(b);
    pipeCloseWrite(b);
}

KTEST(pipeTableExhaustionAndReuse)
{
    int handles[maxPipes + 1];
    int created = 0;

    while (created < maxPipes + 1) {
        int h = pipeCreate();
        if (h < 0)
            break;
        handles[created++] = h;
    }

    KT_LT(t, created, maxPipes + 1);
    KT_EQ(t, pipeCreate(), -1);

    pipeCloseRead(handles[0]);
    pipeCloseWrite(handles[0]);
    int reused = pipeCreate();
    KT_GE(t, reused, 0);

    pipeCloseRead(reused);
    pipeCloseWrite(reused);
    for (int i = 1; i < created; i++) {
        pipeCloseRead(handles[i]);
        pipeCloseWrite(handles[i]);
    }
}
