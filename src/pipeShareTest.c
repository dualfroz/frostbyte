#include "ktest.h"
#include "pipe.h"
#include "vfs.h"
#include "string.h"

/*
 * Coverage for the shared-pipe machinery added this wave (pipe.h's
 * pipeCreateDetached/pipeAddReader/pipeAddWriter/pipeWriteBroken/
 * pipeChannel, plus vfs.h's vfsPipeCreateHandle/vfsPipeAttach/
 * vfsBlockingChannel) -- everything two independently spawned processes
 * need to rendezvous over a real blocking pipe without fork(). The genuine
 * cross-process park/wake handshake itself only runs for real at boot
 * (fifomaster.c spawning fifowriter.c/fiforeader.c, needles
 * "fifomaster: pipeshare ok"/"fiforeader: payload-ok" in tools/ci-test.sh),
 * the same division of labor pipeBlockTest.c already uses for
 * pipeReadBlocking/pipeWriteBlocking: what is deterministic and
 * scheduler-free lives here, what genuinely needs two live execution
 * contexts is proven live.
 */

KTEST(pipeCreateDetachedStartsWithNeitherEndAttached)
{
    int h = pipeCreateDetached();
    KT_GE(t, h, 0);

    /* Nobody has attached a writer yet: a read must report "try again"
       (-1), not spurious end-of-file (0) the way a writers-already-0
       pipeCreate() pipe correctly would. */
    char buffer[8];
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), -1);

    /* Nobody has attached a reader yet either, but a write must still
       succeed (buffered for whenever a reader shows up), not report a
       broken pipe the way a readers-already-0 pipeCreate() pipe would. */
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

    /* Still no writer attached: not EOF. */
    char buffer[4];
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), -1);

    pipeAddWriter(h);
    KT_EQ(t, pipeWrite(h, "z", 1), 1);
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), 1);

    /* Now that a writer really was attached and has left, an empty ring
       means true end-of-file. */
    pipeCloseWrite(h);
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), 0);

    pipeCloseRead(h);
}

KTEST(pipeAddReaderThenCloseMakesRealBrokenPipeNotBeforehand)
{
    int h = pipeCreateDetached();
    KT_GE(t, h, 0);
    pipeAddWriter(h);

    /* No reader attached yet: writing still succeeds, not "broken". */
    KT_FALSE(t, pipeWriteBroken(h));
    KT_EQ(t, pipeWrite(h, "a", 1), 1);

    pipeAddReader(h);
    char buffer[4];
    KT_EQ(t, pipeRead(h, buffer, sizeof(buffer)), 1);

    /* A reader really was attached and has now left: genuinely broken. */
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

    /* An empty, both-ends-attached shared pipe is exactly the "would
       block, but retrying could resolve it" state sysRead's retry path
       (syscall.c) depends on. */
    KT_NOTNULL(t, vfsBlockingChannel(readFd, false));

    KT_EQ(t, vfsWrite(writeFd, "shared", 6), 6);
    char buffer[8];
    memset(buffer, 0, sizeof(buffer));
    KT_EQ(t, vfsRead(readFd, buffer, sizeof(buffer)), 6);
    KT_TRUE(t, memcmp(buffer, "shared", 6) == 0);

    /* Wrong direction / wrong fd kind must never hand back a channel. */
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
    /* vfsPipe's anonymous pipe (both ends in the caller's own table) must
       keep reporting "try again" (-1) to the caller directly, the way
       pipetest.c depends on -- vfsBlockingChannel must never treat it as
       blockable, or sysRead/sysWrite would start parking a single-process
       pipe reader/writer that expects an immediate -1 back. */
    int readFd, writeFd;
    KT_EQ(t, vfsPipe(&readFd, &writeFd), 0);

    char probe[1];
    KT_EQ(t, vfsRead(readFd, probe, 0), 0);
    KT_NULL(t, vfsBlockingChannel(readFd, false));
    KT_NULL(t, vfsBlockingChannel(writeFd, true));

    vfsClose(readFd);
    vfsClose(writeFd);
}
