#include "ktest.h"
#include "pipe.h"
#include "vfs.h"
#include "string.h"

KTEST(pipeCreateDetachedStartsWithNeitherEndAttached)
{
    int h = pipeCreateDetached();
    KT_GE(t, h, 0);

    char buffer[8];
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), -1);

    KT_FALSE(t, pipeWriteBroken(h));
    KT_EQ(t, pipeWrite(h, "hi", 2), 2);

    char got[8];
    KT_EQ(t, pipeRead(h, got, sizeof(got)), 2);
    KT_TRUE(t, memcmp(got, "hi", 2) == 0);

    pipeAddReader(h);
    pipeAddWriter(h);
    pipeCloseRead(h);
    pipeCloseWrite(h);
}

KTEST(pipeAddWriterThenCloseMakesRealEofNotBeforehand)
{
    int h = pipeCreateDetached();
    KT_GE(t, h, 0);
    pipeAddReader(h);

    char buffer[4];
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), -1);

    pipeAddWriter(h);
    KT_EQ(t, pipeWrite(h, "z", 1), 1);
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), 1);

    pipeCloseWrite(h);
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), 0);

    pipeCloseRead(h);
}

KTEST(pipeAddReaderThenCloseMakesRealBrokenPipeNotBeforehand)
{
    int h = pipeCreateDetached();
    KT_GE(t, h, 0);
    pipeAddWriter(h);

    KT_FALSE(t, pipeWriteBroken(h));
    KT_EQ(t, pipeWrite(h, "a", 1), 1);

    pipeAddReader(h);
    char buffer[4];
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), 1);

    pipeCloseRead(h);
    KT_TRUE(t, pipeWriteBroken(h));
    KT_EQ(t, pipeWrite(h, "b", 1), -1);

    pipeCloseWrite(h);
}

KTEST(pipeWriteBrokenAndChannelRejectInvalidHandles)
{
    KT_TRUE(t, pipeWriteBroken(-1));
    KT_TRUE(t, pipeWriteBroken(999));
    KT_NULL(t, pipeChannel(-1));
    KT_NULL(t, pipeChannel(999));

    int h = pipeCreateDetached();
    KT_GE(t, h, 0);
    KT_NOTNULL(t, pipeChannel(h));

    int h2 = pipeCreateDetached();
    KT_GE(t, h2, 0);
    KT_NE(t, pipeChannel(h), pipeChannel(h2));

    pipeAddReader(h);
    pipeAddWriter(h);
    pipeCloseRead(h);
    pipeCloseWrite(h);
    pipeAddReader(h2);
    pipeAddWriter(h2);
    pipeCloseRead(h2);
    pipeCloseWrite(h2);
}

KTEST(vfsPipeCreateHandleAndAttachInstallBlockingFds)
{
    int handle = vfsPipeCreateHandle();
    KT_GE(t, handle, 0);

    int readFd = vfsPipeAttach(handle, false);
    int writeFd = vfsPipeAttach(handle, true);
    KT_GE(t, readFd, 0);
    KT_GE(t, writeFd, 0);
    KT_NE(t, readFd, writeFd);

    KT_NOTNULL(t, vfsBlockingChannel(readFd, false));

    KT_EQ(t, vfsWrite(writeFd, "shared", 6), 6);
    char buffer[8];
    memset(buffer, 0, sizeof(buffer));
    KT_EQ(t, vfsRead(readFd, buffer, sizeof(buffer)), 6);
    KT_TRUE(t, memcmp(buffer, "shared", 6) == 0);

    KT_NULL(t, vfsBlockingChannel(readFd, true));
    KT_NULL(t, vfsBlockingChannel(writeFd, false));
    KT_NULL(t, vfsBlockingChannel(-1, false));

    vfsClose(readFd);
    vfsClose(writeFd);
}

KTEST(vfsPipeAttachRejectsInvalidHandle)
{
    KT_EQ(t, vfsPipeAttach(-1, false), -1);
    KT_EQ(t, vfsPipeAttach(999, true), -1);
}

KTEST(vfsBlockingChannelIsNullForOrdinaryNonBlockingPipe)
{
    int readFd, writeFd;
    KT_EQ(t, vfsPipe(&readFd, &writeFd), 0);

    char probe[1];
    KT_EQ(t, vfsRead(readFd, probe, 0), 0);
    KT_NULL(t, vfsBlockingChannel(readFd, false));
    KT_NULL(t, vfsBlockingChannel(writeFd, true));

    vfsClose(readFd);
    vfsClose(writeFd);
}
