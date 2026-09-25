#include "ktest.h"
#include "pipe.h"
#include "string.h"

KTEST(pipeReadBlockingReturnsQueuedDataImmediately)
{
    int h = pipeCreate();
    KT_GE(t, h, 0);

    const char* msg = "already here";
    KT_EQ(t, pipeWrite(h, msg, strlen(msg)), (int64)strlen(msg));

    char buffer[32];
    memset(buffer, 0, sizeof(buffer));
    int64 got = pipeReadBlocking(h, buffer, sizeof(buffer));
    KT_EQ(t, got, (int64)strlen(msg));
    KT_TRUE(t, memcmp(buffer, msg, strlen(msg)) == 0);

    pipeCloseRead(h);
    pipeCloseWrite(h);
}

KTEST(pipeReadBlockingReturnsEofWithoutParking)
{
    int h = pipeCreate();
    KT_GE(t, h, 0);

    pipeCloseWrite(h);

    char buffer[8];
    KT_EQ(t, pipeReadBlocking(h, buffer, sizeof(buffer)), 0);

    pipeCloseRead(h);
}

KTEST(pipeReadBlockingRejectsInvalidHandleWithoutParking)
{
    char buffer[8];
    KT_EQ(t, pipeReadBlocking(-1, buffer, sizeof(buffer)), -1);
    KT_EQ(t, pipeReadBlocking(999, buffer, sizeof(buffer)), -1);
}

KTEST(pipeWriteBlockingSucceedsImmediatelyWhenRoomExists)
{
    int h = pipeCreate();
    KT_GE(t, h, 0);

    const char* msg = "room to spare";
    int64 written = pipeWriteBlocking(h, msg, strlen(msg));
    KT_EQ(t, written, (int64)strlen(msg));

    char buffer[32];
    memset(buffer, 0, sizeof(buffer));
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), (int64)strlen(msg));

    pipeCloseRead(h);
    pipeCloseWrite(h);
}

KTEST(pipeWriteBlockingOnBrokenPipeReturnsWithoutParking)
{
    int h = pipeCreate();
    KT_GE(t, h, 0);

    pipeCloseRead(h);

    const char* msg = "nobody listening";
    KT_EQ(t, pipeWriteBlocking(h, msg, strlen(msg)), -1);

    pipeCloseWrite(h);
}

KTEST(pipeWriteBlockingRejectsInvalidHandleWithoutParking)
{
    char buffer[4] = { 0 };
    KT_EQ(t, pipeWriteBlocking(-1, buffer, sizeof(buffer)), -1);
    KT_EQ(t, pipeWriteBlocking(999, buffer, sizeof(buffer)), -1);
}

KTEST(pipeWriteBlockingSucceedsRightAfterDrainFreesRoom)
{
    int h = pipeCreate();
    KT_GE(t, h, 0);

    uint8 chunk[pipeBufferSize];
    memset(chunk, 0x5A, sizeof(chunk));

    KT_EQ(t, pipeWrite(h, chunk, sizeof(chunk)), (int64)sizeof(chunk));

    uint8 drained[16];
    KT_EQ(t, pipeRead(h, drained, sizeof(drained)), (int64)sizeof(drained));

    int64 written = pipeWriteBlocking(h, chunk, sizeof(drained));
    KT_EQ(t, written, (int64)sizeof(drained));

    pipeCloseRead(h);
    pipeCloseWrite(h);
}
