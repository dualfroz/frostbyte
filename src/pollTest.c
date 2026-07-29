#include "ktest.h"
#include "poll.h"
#include "vfs.h"
#include "pipe.h"

/*
 * Pure, scheduler-free coverage for vfsPollReady (vfs.c) and its two
 * non-consuming peek helpers (pipeHasData/pipeHasSpace, pipe.h;
 * keyboardReaderHasData, keyboard.h) -- everything that can be driven
 * single-threaded, exactly like pipeShareTest.c/vfsFifoTest.c already do
 * for the machinery poll.h's sysPoll is built on top of. The genuine
 * cross-process block-then-wake handshake (schedule()'s own pollDeadline
 * expiry scan, sched.c; the shared schedPollChannel wake from a SECOND,
 * independently scheduled writer) only runs for real at boot
 * (user/polltest.c + user/pollwriter.c, needles in tools/ci-test.sh) --
 * the same division of labor this file's neighbors already use, and
 * schedTest.c's own doc comment explains why: ktestRunAll() runs before
 * schedStart(), with no second execution context to switch into.
 */

KTEST(vfsPollReadyReportsNvalForClosedOrOutOfRangeFd)
{
    int16 revents = 99;
    KT_TRUE(t, vfsPollReady(-1, pollIn, &revents));
    KT_EQ(t, revents, pollNval);

    revents = 99;
    KT_TRUE(t, vfsPollReady(9999, pollIn | pollOut, &revents));
    KT_EQ(t, revents, pollNval);
}

KTEST(vfsPollReadyPipeReadEndTracksRealData)
{
    int readFd, writeFd;
    KT_EQ(t, vfsPipe(&readFd, &writeFd), 0);

    int16 revents = 99;
    KT_FALSE(t, vfsPollReady(readFd, pollIn, &revents));
    KT_EQ(t, revents, 0);

    KT_EQ(t, vfsWrite(writeFd, "hi", 2), 2);

    revents = 0;
    KT_TRUE(t, vfsPollReady(readFd, pollIn, &revents));
    KT_EQ(t, revents, pollIn);

    /* Asking for pollOut on a read-only end is never satisfiable, no
       matter how much data is queued. */
    revents = 99;
    KT_FALSE(t, vfsPollReady(readFd, pollOut, &revents));
    KT_EQ(t, revents, 0);

    char buffer[8];
    KT_EQ(t, vfsRead(readFd, buffer, sizeof(buffer)), 2);
    vfsClose(readFd);
    vfsClose(writeFd);
}

KTEST(vfsPollReadyPipeWriteEndTracksFreeSpaceThenFull)
{
    int readFd, writeFd;
    KT_EQ(t, vfsPipe(&readFd, &writeFd), 0);

    int16 revents = 0;
    KT_TRUE(t, vfsPollReady(writeFd, pollOut, &revents));
    KT_EQ(t, revents, pollOut);

    /* Fill the ring completely (pipeBufferSize, pipe.h) with non-blocking
       writes -- vfsPipe's ends are exactly the unshared, "-1 means try
       again" kind pipetest.c already depends on, so this can never park
       the calling (only) thread the way a fdFlagBlocking end would. */
    char chunk[64];
    for (int i = 0; i < 64; i++)
        chunk[i] = (char)i;
    int64 written = 0;
    for (int guard = 0; guard < 16; guard++) {
        int64 got = vfsWrite(writeFd, chunk, sizeof(chunk));
        if (got <= 0)
            break;
        written += got;
    }
    KT_TRUE(t, written > 0);
    KT_EQ(t, vfsWrite(writeFd, chunk, sizeof(chunk)), -1);

    revents = 99;
    KT_FALSE(t, vfsPollReady(writeFd, pollOut, &revents));
    KT_EQ(t, revents, 0);

    /* Draining even a little frees space back up. */
    char sink[16];
    KT_EQ(t, vfsRead(readFd, sink, sizeof(sink)), 16);
    revents = 0;
    KT_TRUE(t, vfsPollReady(writeFd, pollOut, &revents));
    KT_EQ(t, revents, pollOut);

    vfsClose(readFd);
    vfsClose(writeFd);
}

KTEST(vfsPollReadyReadEndReportsEofAfterWriterCloses)
{
    int readFd, writeFd;
    KT_EQ(t, vfsPipe(&readFd, &writeFd), 0);

    int16 revents = 0;
    KT_FALSE(t, vfsPollReady(readFd, pollIn, &revents));

    vfsClose(writeFd);

    /* A true EOF pipe is "readable" in real poll(2) too -- the next read
       call returning 0 immediately is exactly what pollIn promises here. */
    revents = 0;
    KT_TRUE(t, vfsPollReady(readFd, pollIn, &revents));
    KT_EQ(t, revents, pollIn);
    char probe[4];
    KT_EQ(t, vfsRead(readFd, probe, sizeof(probe)), 0);

    vfsClose(readFd);
}

KTEST(vfsPollReadyConsoleWriteAlwaysReadyReadNeverWithoutInput)
{
    /* fd 0 (console) is already live for the current (idle) thread by the
       time ktestRunAll() runs -- schedInit() calls vfsFdInit before
       kmain.c ever reaches ktestRunAll(). Every open reader (idle's fd 0
       included) gets a copy of every keystroke keyboardHandleScancode ever
       broadcasts (keyboard.h), including the synthetic ones OTHER test
       files inject (keyboardTest.c's own coverage) -- KTEST execution
       order across files is whatever the ".ktests" linker section happens
       to lay them out in, so idle's fd 0 must be drained here rather than
       assumed empty, or this test's own result would depend on that order. */
    char drain[16];
    while (vfsRead(0, drain, sizeof(drain)) > 0) { }

    int16 revents = 99;
    KT_FALSE(t, vfsPollReady(0, pollIn, &revents));
    KT_EQ(t, revents, 0);

    revents = 0;
    KT_TRUE(t, vfsPollReady(0, pollOut, &revents));
    KT_EQ(t, revents, pollOut);

    revents = 0;
    KT_TRUE(t, vfsPollReady(0, pollIn | pollOut, &revents));
    KT_EQ(t, revents, pollOut);
}
