#include "ktest.h"
#include "poll.h"
#include "vfs.h"
#include "pipe.h"

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

    revents = 0;
    KT_TRUE(t, vfsPollReady(readFd, pollIn, &revents));
    KT_EQ(t, revents, pollIn);
    char probe[4];
    KT_EQ(t, vfsRead(readFd, probe, sizeof(probe)), 0);

    vfsClose(readFd);
}

KTEST(vfsPollReadyConsoleWriteAlwaysReadyReadNeverWithoutInput)
{
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
