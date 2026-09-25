#include "ktest.h"
#include "vfs.h"
#include "pipe.h"
#include "fs.h"
#include "string.h"
#include "klib.h"
#include "sfs.h"

KTEST(vfsMkfifoCreatesFifoNodeVisibleViaStat)
{
    KT_EQ(t, vfsMkfifo("/tmp/fifoBasic"), 0);

    FbStat st;
    KT_EQ(t, vfsStat("/tmp/fifoBasic", &st), 0);
    KT_EQ(t, st.type, fsTypeFifo);

    KT_EQ(t, vfsMkfifo("/tmp/fifoBasic"), -1);

    KT_EQ(t, vfsUnlink("/tmp/fifoBasic"), 0);
    KT_EQ(t, vfsStat("/tmp/fifoBasic", &st), -1);
}

KTEST(vfsMkfifoRejectsReservedAndInvalidPaths)
{
    KT_EQ(t, vfsMkfifo("/dev/disk0"), -1);
    KT_EQ(t, vfsMkfifo("/doesNotExist/fifo"), -1);
    KT_EQ(t, vfsMkfifo("/rootFifo"), -1);
}

KTEST(vfsFifoOpenRejectsDualDirectionModes)
{
    KT_EQ(t, vfsMkfifo("/tmp/fifoModes"), 0);

    KT_EQ(t, vfsOpen("/tmp/fifoModes", oReadWrite), -1);
    KT_EQ(t, vfsOpen("/tmp/fifoModes", oReadWrite | oWriteOnly), -1);

    KT_EQ(t, vfsUnlink("/tmp/fifoModes"), 0);
}

KTEST(vfsFifoRendezvousByPathAloneNoSharedHandle)
{
    KT_EQ(t, vfsMkfifo("/tmp/fifoRendezvous"), 0);

    int rfd = vfsOpen("/tmp/fifoRendezvous", oReadOnly);
    KT_GE(t, rfd, 0);

    char probe[4];
    KT_EQ(t, vfsRead(rfd, probe, sizeof(probe)), -1);
    KT_NOTNULL(t, vfsBlockingChannel(rfd, false));

    int wfd = vfsOpen("/tmp/fifoRendezvous", oWriteOnly);
    KT_GE(t, wfd, 0);
    KT_NE(t, rfd, wfd);

    KT_EQ(t, vfsWrite(wfd, "hi", 2), 2);
    char buffer[4];
    memset(buffer, 0, sizeof(buffer));
    KT_EQ(t, vfsRead(rfd, buffer, sizeof(buffer)), 2);
    KT_TRUE(t, memcmp(buffer, "hi", 2) == 0);

    vfsClose(rfd);
    vfsClose(wfd);
    KT_EQ(t, vfsUnlink("/tmp/fifoRendezvous"), 0);
}

KTEST(vfsFifoUnlinkWhileOpenLeavesLiveFdsWorking)
{
    KT_EQ(t, vfsMkfifo("/tmp/fifoUnlinkLive"), 0);

    int rfd = vfsOpen("/tmp/fifoUnlinkLive", oReadOnly);
    int wfd = vfsOpen("/tmp/fifoUnlinkLive", oWriteOnly);
    KT_GE(t, rfd, 0);
    KT_GE(t, wfd, 0);

    KT_EQ(t, vfsUnlink("/tmp/fifoUnlinkLive"), 0);
    FbStat st;
    KT_EQ(t, vfsStat("/tmp/fifoUnlinkLive", &st), -1);

    KT_EQ(t, vfsWrite(wfd, "ok", 2), 2);
    char buffer[4];
    memset(buffer, 0, sizeof(buffer));
    KT_EQ(t, vfsRead(rfd, buffer, sizeof(buffer)), 2);
    KT_TRUE(t, memcmp(buffer, "ok", 2) == 0);

    vfsClose(rfd);
    vfsClose(wfd);
}

KTEST(vfsUnlinkOnNeverOpenedFifoReclaimsPipeSlot)
{
    char paths[maxPipes][24];
    int madeCount = 0;
    for (int i = 0; i < (int)maxPipes; i++) {
        snprintf(paths[i], sizeof(paths[i]), "/tmp/fifoLeak%d", i);
        if (vfsMkfifo(paths[i]) != 0)
            break;
        madeCount++;
    }
    KT_GT(t, madeCount, 0);

    KT_EQ(t, vfsMkfifo("/tmp/fifoOverflow"), -1);

    KT_EQ(t, vfsUnlink(paths[0]), 0);
    KT_EQ(t, vfsMkfifo("/tmp/fifoOverflow"), 0);

    vfsUnlink("/tmp/fifoOverflow");
    for (int i = 1; i < madeCount; i++)
        vfsUnlink(paths[i]);
}

KTEST(sfsBackedMkfifoCreatesPersistentFifoVisibleViaStat)
{
    KT_TRUE(t, sfsFormat());
    KT_EQ(t, vfsMkfifo("/mnt/fifoBasic"), 0);

    FbStat st;
    KT_EQ(t, vfsStat("/mnt/fifoBasic", &st), 0);
    KT_EQ(t, st.type, fsTypeFifo);
    KT_EQU(t, st.size, 0u);

    KT_EQ(t, vfsMkfifo("/mnt/fifoBasic"), -1);

    KT_EQ(t, vfsUnlink("/mnt/fifoBasic"), 0);
    KT_EQ(t, vfsStat("/mnt/fifoBasic", &st), -1);
}

KTEST(sfsBackedFifoRendezvousByPathAloneNoSharedHandle)
{
    KT_TRUE(t, sfsFormat());
    KT_EQ(t, vfsMkfifo("/mnt/fifoRendezvous"), 0);

    int rfd = vfsOpen("/mnt/fifoRendezvous", oReadOnly);
    KT_GE(t, rfd, 0);

    char probe[4];
    KT_EQ(t, vfsRead(rfd, probe, sizeof(probe)), -1);
    KT_NOTNULL(t, vfsBlockingChannel(rfd, false));

    int wfd = vfsOpen("/mnt/fifoRendezvous", oWriteOnly);
    KT_GE(t, wfd, 0);
    KT_NE(t, rfd, wfd);

    KT_EQ(t, vfsWrite(wfd, "hi", 2), 2);
    char buffer[4];
    memset(buffer, 0, sizeof(buffer));
    KT_EQ(t, vfsRead(rfd, buffer, sizeof(buffer)), 2);
    KT_TRUE(t, memcmp(buffer, "hi", 2) == 0);

    vfsClose(rfd);
    vfsClose(wfd);
    KT_EQ(t, vfsUnlink("/mnt/fifoRendezvous"), 0);
}

KTEST(sfsBackedFifoUnlinkWhileOpenLeavesLiveFdsWorking)
{
    KT_TRUE(t, sfsFormat());
    KT_EQ(t, vfsMkfifo("/mnt/fifoUnlinkLive"), 0);

    int rfd = vfsOpen("/mnt/fifoUnlinkLive", oReadOnly);
    int wfd = vfsOpen("/mnt/fifoUnlinkLive", oWriteOnly);
    KT_GE(t, rfd, 0);
    KT_GE(t, wfd, 0);

    KT_EQ(t, vfsUnlink("/mnt/fifoUnlinkLive"), 0);
    FbStat st;
    KT_EQ(t, vfsStat("/mnt/fifoUnlinkLive", &st), -1);

    KT_EQ(t, vfsWrite(wfd, "ok", 2), 2);
    char buffer[4];
    memset(buffer, 0, sizeof(buffer));
    KT_EQ(t, vfsRead(rfd, buffer, sizeof(buffer)), 2);
    KT_TRUE(t, memcmp(buffer, "ok", 2) == 0);

    vfsClose(rfd);
    vfsClose(wfd);
}

KTEST(sfsBackedFifoEntryPersistsAcrossUnmountRemount)
{
    KT_TRUE(t, sfsFormat());
    KT_EQ(t, vfsMkfifo("/mnt/fifoPersist"), 0);

    sfsUnmount();
    KT_TRUE(t, sfsMount());

    FbStat st;
    KT_EQ(t, vfsStat("/mnt/fifoPersist", &st), 0);
    KT_EQ(t, st.type, fsTypeFifo);

    int rfd = vfsOpen("/mnt/fifoPersist", oReadOnly);
    int wfd = vfsOpen("/mnt/fifoPersist", oWriteOnly);
    KT_GE(t, rfd, 0);
    KT_GE(t, wfd, 0);
    KT_EQ(t, vfsWrite(wfd, "yo", 2), 2);
    char buffer[4];
    memset(buffer, 0, sizeof(buffer));
    KT_EQ(t, vfsRead(rfd, buffer, sizeof(buffer)), 2);
    KT_TRUE(t, memcmp(buffer, "yo", 2) == 0);

    vfsClose(rfd);
    vfsClose(wfd);
    KT_EQ(t, vfsUnlink("/mnt/fifoPersist"), 0);
}

KTEST(sfsBackedFifoReopenAfterCompletedRoundTripGetsFreshWorkingSession)
{
    KT_TRUE(t, sfsFormat());
    KT_EQ(t, vfsMkfifo("/mnt/fifoRoundTrip"), 0);

    int rfd1 = vfsOpen("/mnt/fifoRoundTrip", oReadOnly);
    int wfd1 = vfsOpen("/mnt/fifoRoundTrip", oWriteOnly);
    KT_GE(t, rfd1, 0);
    KT_GE(t, wfd1, 0);
    KT_EQ(t, vfsWrite(wfd1, "one", 3), 3);
    char first[4];
    memset(first, 0, sizeof(first));
    KT_EQ(t, vfsRead(rfd1, first, sizeof(first)), 3);
    KT_TRUE(t, memcmp(first, "one", 3) == 0);
    vfsClose(rfd1);
    vfsClose(wfd1);

    int rfd2 = vfsOpen("/mnt/fifoRoundTrip", oReadOnly);
    KT_GE(t, rfd2, 0);

    char probe[4];
    KT_EQ(t, vfsRead(rfd2, probe, sizeof(probe)), -1);

    int wfd2 = vfsOpen("/mnt/fifoRoundTrip", oWriteOnly);
    KT_GE(t, wfd2, 0);
    KT_EQ(t, vfsWrite(wfd2, "two", 3), 3);
    char second[4];
    memset(second, 0, sizeof(second));
    KT_EQ(t, vfsRead(rfd2, second, sizeof(second)), 3);
    KT_TRUE(t, memcmp(second, "two", 3) == 0);

    vfsClose(rfd2);
    vfsClose(wfd2);
    KT_EQ(t, vfsUnlink("/mnt/fifoRoundTrip"), 0);
}

KTEST(sfsBackedFifoBindingSurvivesReformatOfAnUnrelatedInode)
{
    KT_TRUE(t, sfsFormat());
    KT_EQ(t, vfsMkfifo("/mnt/fifoGenA"), 0);
    int32 firstInode = sfsLookup("/fifoGenA");
    KT_GE(t, firstInode, 0);

    int staleRfd = vfsOpen("/mnt/fifoGenA", oReadOnly);
    KT_GE(t, staleRfd, 0);

    KT_TRUE(t, sfsFormat());
    KT_EQ(t, vfsMkfifo("/mnt/fifoGenA"), 0);
    int32 secondInode = sfsLookup("/fifoGenA");
    KT_GE(t, secondInode, 0);
    KT_EQ(t, secondInode, firstInode);

    int rfd = vfsOpen("/mnt/fifoGenA", oReadOnly);
    int wfd = vfsOpen("/mnt/fifoGenA", oWriteOnly);
    KT_GE(t, rfd, 0);
    KT_GE(t, wfd, 0);
    KT_EQ(t, vfsWrite(wfd, "new", 3), 3);

    char stalePeek[4];
    KT_EQ(t, vfsRead(staleRfd, stalePeek, sizeof(stalePeek)), -1);

    char buffer[4];
    memset(buffer, 0, sizeof(buffer));
    KT_EQ(t, vfsRead(rfd, buffer, sizeof(buffer)), 3);
    KT_TRUE(t, memcmp(buffer, "new", 3) == 0);

    vfsClose(rfd);
    vfsClose(wfd);
    vfsClose(staleRfd);
    vfsUnlink("/mnt/fifoGenA");
}
