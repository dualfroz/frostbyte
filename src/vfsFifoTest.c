#include "ktest.h"
#include "vfs.h"
#include "pipe.h"
#include "fs.h"
#include "string.h"
#include "klib.h"
#include "sfs.h"

/*
 * Coverage for named pipes (vfsMkfifo, vfs.h/vfs.c): a real filesystem entry
 * of type fsTypeFifo that lets two processes rendezvous over a blocking
 * pipe just by agreeing on a path, with no common ancestor and no handle
 * number to pass through argv the way vfsPipeCreateHandle/vfsPipeAttach
 * (pipeShareTest.c) require. The genuine cross-process park/wake handshake
 * over such a fifo only runs for real at boot (namedreader.c/namedwriter.c,
 * two independently spawned kmain.c programs, needles "namedreader:
 * opened, blocking on read"/"namedreader: payload-ok" in tools/ci-test.sh)
 * -- what is deterministic and scheduler-free (open/read/write never
 * actually parking a thread) lives here, matching the division of labor
 * pipeShareTest.c already established for the handle-based flavor.
 */

KTEST(vfsMkfifoCreatesFifoNodeVisibleViaStat)
{
    KT_EQ(t, vfsMkfifo("/tmp/fifoBasic"), 0);

    FbStat st;
    KT_EQ(t, vfsStat("/tmp/fifoBasic", &st), 0);
    KT_EQ(t, st.type, fsTypeFifo);

    /* Same path again must fail, like mkdir/open(O_CREAT|O_EXCL) would --
       mkfifo never silently succeeds on an existing name. */
    KT_EQ(t, vfsMkfifo("/tmp/fifoBasic"), -1);

    KT_EQ(t, vfsUnlink("/tmp/fifoBasic"), 0);
    KT_EQ(t, vfsStat("/tmp/fifoBasic", &st), -1);
}

KTEST(vfsMkfifoRejectsReservedAndInvalidPaths)
{
    /* The one reserved, non-tree path mkfifo never supports -- a raw byte
       device has no notion of a named directory entry at all, unlike "/mnt"
       (see the sfsCreateFifo-backed tests below), which does. */
    KT_EQ(t, vfsMkfifo("/dev/disk0"), -1);
    /* Parent directory does not exist at all. */
    KT_EQ(t, vfsMkfifo("/doesNotExist/fifo"), -1);
    /* Root itself is never writable -- only vfsMountTmpfs-created
       directories (e.g. "/tmp") are, same rule makeChildFile enforces for
       oCreate. */
    KT_EQ(t, vfsMkfifo("/rootFifo"), -1);
}

KTEST(vfsFifoOpenRejectsDualDirectionModes)
{
    KT_EQ(t, vfsMkfifo("/tmp/fifoModes"), 0);

    /* This kernel's pipe end is inherently one-directional (fdPipeRead XOR
       fdPipeWrite) -- a fifo fd can never be both, unlike Linux's permissive
       (POSIX-undefined) O_RDWR on a FIFO. */
    KT_EQ(t, vfsOpen("/tmp/fifoModes", oReadWrite), -1);
    KT_EQ(t, vfsOpen("/tmp/fifoModes", oReadWrite | oWriteOnly), -1);

    KT_EQ(t, vfsUnlink("/tmp/fifoModes"), 0);
}

KTEST(vfsFifoRendezvousByPathAloneNoSharedHandle)
{
    KT_EQ(t, vfsMkfifo("/tmp/fifoRendezvous"), 0);

    /* Two independent vfsOpen calls on the SAME PATH -- no handle number, no
       argv, no common caller -- is exactly what namedreader.c/namedwriter.c
       (two kmain.c boot programs with no parent-child relationship) rely
       on. */
    int rfd = vfsOpen("/tmp/fifoRendezvous", oReadOnly);
    KT_GE(t, rfd, 0);

    /* Nobody has opened the write end yet: a read must report "try again"
       (-1), never spurious end-of-file, exactly like a freshly
       pipeCreateDetached handle (pipeShareTest.c). */
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

    /* Real unlink-while-open semantics: removing the name must not disturb
       fds opened before the unlink -- they keep working off their own copy
       of the raw pipe handle, never the freed vfs node. */
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
    /* Drives the pipe table (pipe.h's fixed-size `pipes[maxPipes]`, shared
       with every other pipe user in the kernel) to exhaustion using fifos
       that are never opened by anyone -- the one case pipeCloseRead/
       pipeCloseWrite's own cleanup can never reach, since neither is ever
       called for an end nobody attached. Does not assume the table starts
       fully empty (kmain.c's own vfsMkfifo("/tmp/rendezvous") already holds
       one slot for the rest of boot by the time ktests run) -- just proves
       whatever headroom exists gets used up, and reclaimed, correctly. */
    char paths[maxPipes][24];
    int madeCount = 0;
    for (int i = 0; i < (int)maxPipes; i++) {
        snprintf(paths[i], sizeof(paths[i]), "/tmp/fifoLeak%d", i);
        if (vfsMkfifo(paths[i]) != 0)
            break;
        madeCount++;
    }
    KT_GT(t, madeCount, 0);

    /* Table is now genuinely full: one more must fail. */
    KT_EQ(t, vfsMkfifo("/tmp/fifoOverflow"), -1);

    /* Unlinking one never-opened fifo must reclaim its slot immediately
       (pipeDestroyIfUnattached), not leave it dangling until some later,
       unrelated event. */
    KT_EQ(t, vfsUnlink(paths[0]), 0);
    KT_EQ(t, vfsMkfifo("/tmp/fifoOverflow"), 0);

    /* Leave the shared pipe table exactly as free as it was found, so later
       boot-time pipe users (the pipeProducer/pipeConsumer demo,
       fifomaster's handle, killtest's shared pipe, namedreader/
       namedwriter's own fifo) are never starved by this test having run. */
    vfsUnlink("/tmp/fifoOverflow");
    for (int i = 1; i < madeCount; i++)
        vfsUnlink(paths[i]);
}

/*
 * Coverage for PERSISTENT named pipes: vfsMkfifo under "/mnt" now creates a
 * real, on-disk sfs.h entry (sfsTypeFifo) instead of being rejected, so the
 * special file itself -- unlike everything above, which lives only in this
 * process's kmalloc heap -- survives sfsUnmount()+sfsMount(). The live
 * pipe.h byte stream two openers exchange data through is still purely
 * in-memory (vfs.c's sfsFifoHandleFor), created lazily on first open, since
 * buffered bytes never persist across a real mkfifo(2)'s reboot either --
 * only the rendezvous point's name and type do. Every test below starts
 * with its own sfsFormat() (same discipline sfsTest.c uses) so results
 * never depend on whatever an earlier test (here or in sfsTest.c, same
 * boot, same disk) left behind.
 */

KTEST(sfsBackedMkfifoCreatesPersistentFifoVisibleViaStat)
{
    KT_TRUE(t, sfsFormat());
    KT_EQ(t, vfsMkfifo("/mnt/fifoBasic"), 0);

    FbStat st;
    KT_EQ(t, vfsStat("/mnt/fifoBasic", &st), 0);
    KT_EQ(t, st.type, fsTypeFifo);
    KT_EQU(t, st.size, 0u);

    /* Same path again must fail, exactly like the tmpfs case above. */
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

    /* Nobody has opened the write end yet: "try again" (-1), never spurious
       end-of-file, exactly like the tmpfs fifo and a freshly
       pipeCreateDetached handle both already guarantee. */
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

    /* Real unlink-while-open semantics, same guarantee as the tmpfs fifo:
       removing the directory entry must not disturb fds opened before the
       unlink -- they keep working off their own copy of the raw pipe
       handle number. */
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

    /* The real point of this test (mirrors sfsTest.c's own
       sfsMountReloadsExistingFilesystem-style coverage): drop EVERY
       in-memory sfs.c structure (bitmaps, superblock) and reconstruct them
       purely from what sfsFormat/sfsCreateFifo actually wrote to disk --
       proving the fifo is a genuine on-disk entry, not a kmalloc node that
       would evaporate the instant this process's heap did (like the tmpfs
       fifo above, which vfsMkfifo("/mnt/...") deliberately never was). */
    sfsUnmount();
    KT_TRUE(t, sfsMount());

    FbStat st;
    KT_EQ(t, vfsStat("/mnt/fifoPersist", &st), 0);
    KT_EQ(t, st.type, fsTypeFifo);

    /* Still fully functional after the remount, not just present in a
       directory listing: a fresh open/write/read round trip works exactly
       as it would have before the remount. */
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
    /* Ordinary fifo usage, not a corner case: open both ends, exchange
       data, close both, then reopen the SAME persistent path later for a
       second, unrelated conversation -- exactly what a real named pipe is
       for. The first round trip fully releases pipe.h's underlying slot
       back to its general pool (pipeReleaseIfUnused, once both readers and
       writers hit 0), so the second round's vfs.c-level binding
       (sfsFifoHandleFor) MUST detect that its remembered handle number no
       longer refers to the same pipe (pipeIncarnation, pipe.h) and mint a
       genuinely fresh one -- otherwise this second write would silently
       target a slot nobody has attached to (or, worse, one some unrelated
       pipe elsewhere in the kernel grabbed in between), and would report
       -1 forever instead of actually delivering the byte. */
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

    /* Second, independent session on the same path. */
    int rfd2 = vfsOpen("/mnt/fifoRoundTrip", oReadOnly);
    KT_GE(t, rfd2, 0);

    /* A genuinely fresh pipe reports "try again" here, exactly like a
       first-ever open -- a stale, no-longer-attached handle would instead
       report -1 forever for an entirely different reason (pipeAt finding
       nothing "used" at that slot), which the write assertion below is
       what actually tells apart from this one. */
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
    /* sfsTest.c's tests each start with their own sfsFormat(), reusing
       inode numbers dozens of times a boot. A fifo binding left open
       ACROSS such a reformat (contrived here, but exactly the shape
       reclaimOpenFds/sched.c would leave behind for a thread that died
       without closing its fd -- see sched.c) must never let the fresh
       filesystem's completely unrelated reuse of the same inode number
       inherit the old, orphaned session. */
    KT_TRUE(t, sfsFormat());
    KT_EQ(t, vfsMkfifo("/mnt/fifoGenA"), 0);
    int32 firstInode = sfsLookup("/fifoGenA");
    KT_GE(t, firstInode, 0);

    /* Left deliberately open across the reformat below -- an orphaned
       session, on purpose, is exactly what must NOT leak into the new one. */
    int staleRfd = vfsOpen("/mnt/fifoGenA", oReadOnly);
    KT_GE(t, staleRfd, 0);

    /* A full reformat: every old inode number, including firstInode, now
       means whatever the fresh, empty filesystem below reassigns it to. */
    KT_TRUE(t, sfsFormat());
    KT_EQ(t, vfsMkfifo("/mnt/fifoGenA"), 0);
    int32 secondInode = sfsLookup("/fifoGenA");
    KT_GE(t, secondInode, 0);
    /* Deterministic on a freshly formatted, otherwise-empty filesystem:
       both are the very first entry created after root, so the numbers
       collide -- exactly the scenario that makes the generation check
       necessary rather than merely defensive. */
    KT_EQ(t, secondInode, firstInode);

    int rfd = vfsOpen("/mnt/fifoGenA", oReadOnly);
    int wfd = vfsOpen("/mnt/fifoGenA", oWriteOnly);
    KT_GE(t, rfd, 0);
    KT_GE(t, wfd, 0);
    KT_EQ(t, vfsWrite(wfd, "new", 3), 3);

    /* The discriminating check: if the binding table wrongly inherited the
       pre-reformat session (generation check not actually doing its job),
       staleRfd would be a SECOND reader attached to the very same
       underlying pipe wfd just wrote into, and -- read before rfd below --
       would steal the payload meant for the new session's own reader.
       Correct isolation means staleRfd's pipe never had any writer at all
       (it only ever attached as a reader), so it must still report "try
       again", never the new session's bytes. */
    char stalePeek[4];
    KT_EQ(t, vfsRead(staleRfd, stalePeek, sizeof(stalePeek)), -1);

    char buffer[4];
    memset(buffer, 0, sizeof(buffer));
    KT_EQ(t, vfsRead(rfd, buffer, sizeof(buffer)), 3);
    KT_TRUE(t, memcmp(buffer, "new", 3) == 0);

    vfsClose(rfd);
    vfsClose(wfd);
    /* The orphaned pre-reformat fd is cleaned up here rather than left to
       leak a pipe.c slot for the rest of the boot -- vfsClose only ever
       touches pipe.c state (pipeCloseRead), never re-derefs the long-gone
       inode, so this is safe regardless of what the fresh filesystem did
       with the number. */
    vfsClose(staleRfd);
    vfsUnlink("/mnt/fifoGenA");
}
