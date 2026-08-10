#include "ktest.h"
#include "sfs.h"
#include "ata.h"
#include "string.h"
#include "heap.h"
#include "console.h"
#include "rtc.h"
#include "fs.h"

/*
 * Every test starts with an explicit sfsFormat() so results do not depend on
 * whatever build/disk.img happened to contain from a previous `make test`
 * run (the disk image is not recreated between runs unless deleted). That
 * keeps the gate deterministic: a stale leftover file on disk must never be
 * the reason a check passes or fails.
 */

KTEST(sfsFormatAndMountAreReal)
{
    if (!KT_TRUE(t, ataPresent()))
        return;
    KT_TRUE(t, sfsFormat());
    KT_TRUE(t, sfsMounted());
    KT_GT(t, sfsCapacityBytes(), 1024 * 1024);

    int32 root = sfsLookup("");
    KT_EQ(t, root, sfsRootInode);
    KT_EQ(t, sfsTypeOf(root), sfsTypeDir);
    KT_EQ(t, sfsSizeOf(root), 0);
}

KTEST(sfsCreateFileRoundTrip)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("hello.txt");
    KT_TRUE(t, f > 0);
    KT_EQ(t, sfsTypeOf(f), sfsTypeFile);
    KT_EQ(t, sfsSizeOf(f), 0);

    const char* msg = "hello, disk!";
    uint64 len = strlen(msg);
    KT_EQ(t, sfsWriteAt(f, 0, msg, len), (int64)len);
    KT_EQ(t, sfsSizeOf(f), len);

    char back[32];
    memset(back, 0, sizeof(back));
    KT_EQ(t, sfsReadAt(f, 0, back, len), (int64)len);
    KT_EQ(t, memcmp(back, msg, len), 0);
}

KTEST(sfsOverwriteReflectsLatestData)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("a.bin");
    KT_TRUE(t, f > 0);
    KT_EQ(t, sfsWriteAt(f, 0, "AAAA", 4), 4);
    KT_EQ(t, sfsWriteAt(f, 0, "BB", 2), 2);

    char back[4];
    KT_EQ(t, sfsReadAt(f, 0, back, 4), 4);
    KT_EQ(t, memcmp(back, "BBAA", 4), 0);
    KT_EQ(t, sfsSizeOf(f), 4);
}

KTEST(sfsWritePastEndZeroFillsGap)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("gap.bin");
    KT_TRUE(t, f > 0);
    KT_EQ(t, sfsWriteAt(f, 10, "X", 1), 1);
    KT_EQ(t, sfsSizeOf(f), 11);

    uint8 back[11];
    KT_EQ(t, sfsReadAt(f, 0, back, 11), 11);
    for (int i = 0; i < 10; i++)
        KT_EQ(t, back[i], 0);
    KT_EQ(t, back[10], 'X');
}

KTEST(sfsMultiBlockFileSpansDirectPointers)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("multi.bin");
    KT_TRUE(t, f > 0);

    /* Big enough to span several blocks: kmalloc'd rather than kept on the
       stack, matching the rbt/slab stress-test convention (this ktest
       battery runs on the fixed 16 KiB boot stack, not a per-thread one). */
    #define sfsTestSpan (3 * sfsBlockSize + 200)
    uint8* pattern = (uint8*)kmalloc(sfsTestSpan);
    uint8* back = (uint8*)kmalloc(sfsTestSpan);
    KT_NOTNULL(t, pattern);
    KT_NOTNULL(t, back);
    if (!pattern || !back) {
        kfree(pattern);
        kfree(back);
        return;
    }
    for (int i = 0; i < sfsTestSpan; i++)
        pattern[i] = (uint8)(i * 31 + 7);

    KT_EQ(t, sfsWriteAt(f, 0, pattern, sfsTestSpan), sfsTestSpan);
    KT_EQ(t, sfsSizeOf(f), sfsTestSpan);

    memset(back, 0, sfsTestSpan);
    KT_EQ(t, sfsReadAt(f, 0, back, sfsTestSpan), sfsTestSpan);
    KT_EQ(t, memcmp(back, pattern, sfsTestSpan), 0);

    kfree(pattern);
    kfree(back);
    #undef sfsTestSpan
}

KTEST(sfsFileGrowsThroughIndirectBlock)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("big.bin");
    KT_TRUE(t, f > 0);

    /* 10 direct blocks (5120 bytes) plus a chunk that only the single
       indirect block can address: this must reach past sfsDirectBlocks.
       kmalloc'd (three of these at once would be ~18 KiB, more than the
       whole 16 KiB boot stack ktest runs on). */
    #define sfsBigSpan (12 * sfsBlockSize + 33)
    uint8* pattern = (uint8*)kmalloc(sfsBigSpan);
    uint8* back = (uint8*)kmalloc(sfsBigSpan);
    uint8* verify = (uint8*)kmalloc(sfsBigSpan);
    KT_NOTNULL(t, pattern);
    KT_NOTNULL(t, back);
    KT_NOTNULL(t, verify);
    if (!pattern || !back || !verify) {
        kfree(pattern);
        kfree(back);
        kfree(verify);
        return;
    }
    for (int i = 0; i < sfsBigSpan; i++)
        pattern[i] = (uint8)(i ^ 0x5A);

    KT_EQ(t, sfsWriteAt(f, 0, pattern, sfsBigSpan), sfsBigSpan);
    KT_EQ(t, sfsSizeOf(f), sfsBigSpan);

    memset(back, 0, sfsBigSpan);
    KT_EQ(t, sfsReadAt(f, 0, back, sfsBigSpan), sfsBigSpan);
    KT_EQ(t, memcmp(back, pattern, sfsBigSpan), 0);

    /* Overwrite a slice that straddles the direct/indirect boundary and
       confirm only that slice changes. */
    uint8 patch[64];
    memset(patch, 0xEE, sizeof(patch));
    uint64 patchOffset = 10 * sfsBlockSize - 20;
    KT_EQ(t, sfsWriteAt(f, patchOffset, patch, sizeof(patch)), (int64)sizeof(patch));

    KT_EQ(t, sfsReadAt(f, 0, verify, sfsBigSpan), sfsBigSpan);
    KT_EQ(t, memcmp(verify + patchOffset, patch, sizeof(patch)), 0);
    KT_EQ(t, memcmp(verify, pattern, patchOffset), 0);

    kfree(pattern);
    kfree(back);
    kfree(verify);
    #undef sfsBigSpan
}

KTEST(sfsNestedDirectoriesAndLookup)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateDir("etc") > 0);
    KT_TRUE(t, sfsCreateDir("etc/sub") > 0);
    int32 f = sfsCreateFile("etc/sub/leaf.txt");
    KT_TRUE(t, f > 0);
    KT_EQ(t, sfsWriteAt(f, 0, "leaf", 4), 4);

    int32 viaLookup = sfsLookup("etc/sub/leaf.txt");
    KT_EQ(t, viaLookup, f);
    int32 subDir = sfsLookup("etc/sub");
    KT_TRUE(t, subDir > 0);
    KT_EQ(t, sfsTypeOf(subDir), sfsTypeDir);
    KT_EQ(t, sfsLookup("etc/nope"), -1);
    KT_EQ(t, sfsLookup("nope/at/all"), -1);
}

KTEST(sfsDuplicateCreateRejected)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateFile("dup.txt") > 0);
    KT_EQ(t, sfsCreateFile("dup.txt"), -1);
    KT_EQ(t, sfsCreateDir("dup.txt"), -1);
}

KTEST(sfsRemoveFileFreesBlocksForReuse)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("tofree.bin");
    KT_TRUE(t, f > 0);
    #define sfsFreeChunk (4 * sfsBlockSize)
    uint8* chunk = (uint8*)kmalloc(sfsFreeChunk);
    KT_NOTNULL(t, chunk);
    if (!chunk)
        return;
    memset(chunk, 0x42, sfsFreeChunk);
    KT_EQ(t, sfsWriteAt(f, 0, chunk, sfsFreeChunk), sfsFreeChunk);
    kfree(chunk);
    #undef sfsFreeChunk

    KT_TRUE(t, sfsRemove("tofree.bin"));
    KT_EQ(t, sfsLookup("tofree.bin"), -1);

    /* A same-size file created afterward must succeed, proving the blocks
       (and the inode slot) actually came back to the free pool rather than
       leaking on every create/remove cycle. */
    int32 again = sfsCreateFile("tofree.bin");
    KT_TRUE(t, again > 0);
    KT_EQ(t, sfsSizeOf(again), 0);
}

KTEST(sfsRmdirRequiresEmpty)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateDir("full") > 0);
    KT_TRUE(t, sfsCreateFile("full/child.txt") > 0);
    KT_FALSE(t, sfsRemove("full"));

    KT_TRUE(t, sfsRemove("full/child.txt"));
    KT_TRUE(t, sfsRemove("full"));
    KT_EQ(t, sfsLookup("full"), -1);
}

KTEST(sfsReaddirListsAllEntriesAndSwapRemoveKeepsRest)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 dir = sfsCreateDir("listing");
    KT_TRUE(t, dir > 0);
    KT_TRUE(t, sfsCreateFile("listing/a") > 0);
    KT_TRUE(t, sfsCreateFile("listing/b") > 0);
    KT_TRUE(t, sfsCreateFile("listing/c") > 0);

    uint32 seen = 0;
    char name[sfsNameMax + 1];
    uint32 type;
    for (uint32 i = 0; sfsReaddirAt(dir, i, name, sizeof(name), &type); i++) {
        KT_EQ(t, type, sfsTypeFile);
        seen++;
    }
    KT_EQ(t, seen, 3);

    KT_TRUE(t, sfsRemove("listing/b"));
    KT_NE(t, sfsLookup("listing/a"), -1);
    KT_NE(t, sfsLookup("listing/c"), -1);
    KT_EQ(t, sfsLookup("listing/b"), -1);

    seen = 0;
    for (uint32 i = 0; sfsReaddirAt(dir, i, name, sizeof(name), &type); i++)
        seen++;
    KT_EQ(t, seen, 2);
}

KTEST(sfsTruncateShrinksAndFreesBlocks)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("shrink.bin");
    KT_TRUE(t, f > 0);
    #define sfsShrinkChunk (5 * sfsBlockSize)
    uint8* chunk = (uint8*)kmalloc(sfsShrinkChunk);
    KT_NOTNULL(t, chunk);
    if (!chunk)
        return;
    memset(chunk, 0x7A, sfsShrinkChunk);
    KT_EQ(t, sfsWriteAt(f, 0, chunk, sfsShrinkChunk), sfsShrinkChunk);

    KT_TRUE(t, sfsTruncate(f, sfsBlockSize));
    KT_EQ(t, sfsSizeOf(f), sfsBlockSize);

    uint8 back[sfsBlockSize];
    KT_EQ(t, sfsReadAt(f, 0, back, sfsBlockSize), sfsBlockSize);
    KT_EQ(t, memcmp(back, chunk, sfsBlockSize), 0);
    kfree(chunk);
    #undef sfsShrinkChunk

    KT_TRUE(t, sfsTruncate(f, 0));
    KT_EQ(t, sfsSizeOf(f), 0);
}

KTEST(sfsPersistsAcrossUnmountRemount)
{
    /* The real point of this test: sfsUnmount() drops every in-memory
       structure (bitmaps freed, mounted flag cleared). If sfsMount() can
       still find the file and its exact bytes afterward, the filesystem is
       genuinely backed by disk, not by a cache that happens to survive. */
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateDir("keep") > 0);
    int32 f = sfsCreateFile("keep/data.bin");
    KT_TRUE(t, f > 0);
    const char* payload = "this must survive a real remount";
    uint64 len = strlen(payload);
    KT_EQ(t, sfsWriteAt(f, 0, payload, len), (int64)len);

    sfsUnmount();
    KT_FALSE(t, sfsMounted());

    KT_TRUE(t, sfsMount());
    KT_TRUE(t, sfsMounted());

    int32 reloaded = sfsLookup("keep/data.bin");
    KT_EQ(t, reloaded, f);
    KT_EQ(t, sfsSizeOf(reloaded), len);

    char back[64];
    memset(back, 0, sizeof(back));
    KT_EQ(t, sfsReadAt(reloaded, 0, back, len), (int64)len);
    KT_EQ(t, memcmp(back, payload, len), 0);
}

KTEST(sfsMountWithoutFormatRecoversPriorState)
{
    /* sfsMount() on an already-formatted disk must NOT reformat: a second
       mount call has to see the first mount's data, exactly like a real
       reboot re-mounting an existing filesystem. */
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("survives.txt");
    KT_TRUE(t, f > 0);
    KT_EQ(t, sfsWriteAt(f, 0, "still here", 10), 10);
    sfsUnmount();

    KT_TRUE(t, sfsMount());
    int32 found = sfsLookup("survives.txt");
    KT_EQ(t, found, f);
    char back[10];
    KT_EQ(t, sfsReadAt(found, 0, back, 10), 10);
    KT_EQ(t, memcmp(back, "still here", 10), 0);
}

/*
 * mtime/ctime coverage below. Every bound uses a real rtcNow() reading taken
 * strictly before or after the operation under test, in this same thread, so
 * "before <= stamped-value <= after" is a real chronological fact (the clock
 * only ever moves forward), never a flaky timing assumption -- unlike a
 * cross-run "did it increase" check, which would need a real elapsed second
 * to be meaningful. sfsLinkBumpsCtimeButNeverTouchesMtime instead checks
 * exact, timing-independent equality: the link code path must never write
 * inode.mtime at all, so it has to come back byte-for-byte unchanged
 * regardless of how much wall-clock time passed.
 */

KTEST(sfsCreateFileStampsMtimeAndCtimeNearNow)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int64 before = rtcNow();
    int32 f = sfsCreateFile("stamped.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    int64 after = rtcNow();

    uint64 mtime = sfsMtimeOf(f);
    uint64 ctime = sfsCtimeOf(f);
    KT_EQ(t, mtime, ctime);
    KT_GE(t, mtime, before);
    KT_LE(t, mtime, after);
}

KTEST(sfsWriteAtBumpsMtimeAndCtimeStayingInSync)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("written.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    uint64 createdMtime = sfsMtimeOf(f);

    int64 before = rtcNow();
    const char* payload = "sfs mtime test payload";
    uint64 len = strlen(payload);
    KT_EQ(t, sfsWriteAt(f, 0, payload, len), (int64)len);
    int64 after = rtcNow();

    uint64 mtime = sfsMtimeOf(f);
    uint64 ctime = sfsCtimeOf(f);
    KT_EQ(t, mtime, ctime);
    KT_GE(t, mtime, createdMtime);
    KT_GE(t, mtime, before);
    KT_LE(t, mtime, after);
}

KTEST(sfsLinkBumpsCtimeButNeverTouchesMtime)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("original.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    KT_EQ(t, sfsWriteAt(f, 0, "hi", 2), 2);

    uint64 mtimeBeforeLink = sfsMtimeOf(f);
    uint64 ctimeBeforeLink = sfsCtimeOf(f);

    int64 before = rtcNow();
    KT_EQ(t, sfsLink("original.txt", "alias.txt"), f);
    int64 after = rtcNow();

    KT_EQ(t, sfsMtimeOf(f), mtimeBeforeLink);
    KT_GE(t, sfsCtimeOf(f), ctimeBeforeLink);
    KT_GE(t, sfsCtimeOf(f), before);
    KT_LE(t, sfsCtimeOf(f), after);
}

KTEST(sfsRemoveOneOfTwoHardlinksBumpsRemainingLinkCtime)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("keepme.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    KT_EQ(t, sfsLink("keepme.txt", "alias2.txt"), f);
    KT_EQ(t, sfsLinkCountOf(f), 2u);

    uint64 ctimeBeforeRemove = sfsCtimeOf(f);
    int64 before = rtcNow();
    KT_TRUE(t, sfsRemove("alias2.txt"));
    int64 after = rtcNow();

    KT_EQ(t, sfsLinkCountOf(f), 1u);
    KT_GE(t, sfsCtimeOf(f), ctimeBeforeRemove);
    KT_GE(t, sfsCtimeOf(f), before);
    KT_LE(t, sfsCtimeOf(f), after);
}

KTEST(sfsPersistsMtimeAndCtimeAcrossUnmountRemount)
{
    /* Same real point as sfsPersistsAcrossUnmountRemount above, applied to
       the new fields: sfsUnmount() drops every in-memory structure, so an
       exact match after sfsMount() proves these timestamps genuinely live on
       disk inside the inode, not in some cache that happens to survive. */
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("timepersist.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    KT_EQ(t, sfsWriteAt(f, 0, "persisted", 9), 9);

    uint64 mtimeBefore = sfsMtimeOf(f);
    uint64 ctimeBefore = sfsCtimeOf(f);
    KT_GT(t, mtimeBefore, 0);

    sfsUnmount();
    KT_TRUE(t, sfsMount());

    int32 reloaded = sfsLookup("timepersist.txt");
    KT_EQ(t, reloaded, f);
    KT_EQ(t, sfsMtimeOf(reloaded), mtimeBefore);
    KT_EQ(t, sfsCtimeOf(reloaded), ctimeBefore);
}

/* Real chmod(2) owner rwx bits (fs.h's fsModeOwner*): createNode stamps a
   type-based default (a file gets fsModeDefaultFile, a directory gets
   fsModeDefaultDir), sfsChmod overwrites it and bumps ctime like any other
   metadata-only mutation (sfsLink's linkCount bump does the same, see
   above), and sfsModeOf/sfsChmod on an invalid inode number fail the same
   way sfsSizeOf/sfsTypeOf already do for one. */
KTEST(sfsModeDefaultsByTypeAndChmodBumpsCtime)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("modefile.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    KT_EQ(t, sfsModeOf(f), fsModeDefaultFile);

    int32 d = sfsCreateDir("modedir");
    if (!KT_TRUE(t, d > 0))
        return;
    KT_EQ(t, sfsModeOf(d), fsModeDefaultDir);

    uint64 ctimeBefore = sfsCtimeOf(f);
    int64 before = rtcNow();
    KT_TRUE(t, sfsChmod(f, 0400));
    int64 after = rtcNow();

    KT_EQ(t, sfsModeOf(f), 0400u);
    KT_GE(t, sfsCtimeOf(f), ctimeBefore);
    KT_GE(t, sfsCtimeOf(f), before);
    KT_LE(t, sfsCtimeOf(f), after);

    KT_EQ(t, sfsModeOf(-1), 0u);
    KT_EQ(t, sfsModeOf(9999), 0u);
    KT_FALSE(t, sfsChmod(-1, 0644));
    KT_FALSE(t, sfsChmod(9999, 0644));
}

/* Same real point as sfsPersistsMtimeAndCtimeAcrossUnmountRemount above,
   applied to mode: sfsUnmount() drops every in-memory structure, so an
   exact match after sfsMount() proves the chmod'd value genuinely lives on
   disk inside the inode, not in some cache that happens to survive. */
KTEST(sfsPersistsModeAcrossUnmountRemount)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("modepersist.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    KT_TRUE(t, sfsChmod(f, 0400));

    sfsUnmount();
    KT_TRUE(t, sfsMount());

    int32 reloaded = sfsLookup("modepersist.txt");
    KT_EQ(t, reloaded, f);
    KT_EQ(t, sfsModeOf(reloaded), 0400u);
}

/* Real owner identity (fs.h's FbStat.uid, sched.h's currentThreadUid()):
   createNode stamps every new node -- file, dir, symlink, fifo all go
   through it, exactly like mtime/ctime already verified sufficient for
   (FALA 39's own note) -- with whoever is CURRENT at creation time.
   ktestRunAll always runs as the permanent idle thread (id 0, uid 0), the
   same context schedTest.c's own uid-default tests already document
   relying on, so both a freshly created file and the root inode itself
   (sfsFormat, this test's own sfsFormat() call included) come back
   root-owned. sfsUidOf on an invalid inode number fails the same way
   sfsModeOf/sfsSizeOf/sfsTypeOf already do for one. */
KTEST(sfsCreateNodeStampsCreatorUidAsOwner)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_EQU(t, sfsUidOf(sfsRootInode), 0u);

    int32 f = sfsCreateFile("uidfile.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    KT_EQU(t, sfsUidOf(f), 0u);

    int32 d = sfsCreateDir("uiddir");
    if (!KT_TRUE(t, d > 0))
        return;
    KT_EQU(t, sfsUidOf(d), 0u);

    KT_EQU(t, sfsUidOf(-1), 0u);
    KT_EQU(t, sfsUidOf(9999), 0u);
}

/* Same real point as sfsPersistsModeAcrossUnmountRemount above, applied to
   uid: sfsUnmount() drops every in-memory structure, so an exact match
   after sfsMount() proves ownership genuinely lives on disk inside the
   inode, not in some cache that happens to survive. */
KTEST(sfsPersistsUidAcrossUnmountRemount)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("uidpersist.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    KT_EQU(t, sfsUidOf(f), 0u);

    sfsUnmount();
    KT_TRUE(t, sfsMount());

    int32 reloaded = sfsLookup("uidpersist.txt");
    KT_EQ(t, reloaded, f);
    KT_EQU(t, sfsUidOf(reloaded), 0u);
}

/* Real chown(2)'s on-disk half (sfsChown): overwrites the inode's owner
   uid outright and bumps ctime like sfsChmod's own metadata-only mutation
   does -- same "bump ctime, invalid inode fails the same way" shape as
   sfsModeDefaultsByTypeAndChmodBumpsCtime above. sfsChown itself has no
   privilege check (vfsChown, vfs.c, is the real choke point that enforces
   chown(2)'s root-only rule), so calling it directly here at any uid is
   deliberately not restricted. */
KTEST(sfsChownReassignsOwnerAndBumpsCtime)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("chownfile.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    KT_EQU(t, sfsUidOf(f), 0u);

    uint64 ctimeBefore = sfsCtimeOf(f);
    int64 before = rtcNow();
    KT_TRUE(t, sfsChown(f, 9001));
    int64 after = rtcNow();

    KT_EQU(t, sfsUidOf(f), 9001u);
    KT_GE(t, sfsCtimeOf(f), ctimeBefore);
    KT_GE(t, sfsCtimeOf(f), before);
    KT_LE(t, sfsCtimeOf(f), after);

    KT_FALSE(t, sfsChown(-1, 1000));
    KT_FALSE(t, sfsChown(9999, 1000));
}

/* Same real point as sfsPersistsUidAcrossUnmountRemount above, but for a
   uid REASSIGNED by sfsChown after creation, not just the creation-time
   default -- proves the chown write path itself lands on disk, not just
   that createNode's own initial stamp does. */
KTEST(sfsPersistsChownedUidAcrossUnmountRemount)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("chownpersist.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    KT_TRUE(t, sfsChown(f, 8123));

    sfsUnmount();
    KT_TRUE(t, sfsMount());

    int32 reloaded = sfsLookup("chownpersist.txt");
    KT_EQ(t, reloaded, f);
    KT_EQU(t, sfsUidOf(reloaded), 8123u);
}

/* Real chgrp(2)'s on-disk half (sfsChgrp), mirroring
   sfsChownReassignsOwnerAndBumpsCtime exactly one field over -- same "no
   privilege check of its own, ctime bumps, invalid inode fails the same
   way" shape. */
KTEST(sfsChgrpReassignsGroupAndBumpsCtime)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("chgrpfile.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    KT_EQU(t, sfsGidOf(f), 0u);

    uint64 ctimeBefore = sfsCtimeOf(f);
    int64 before = rtcNow();
    KT_TRUE(t, sfsChgrp(f, 9002));
    int64 after = rtcNow();

    KT_EQU(t, sfsGidOf(f), 9002u);
    KT_GE(t, sfsCtimeOf(f), ctimeBefore);
    KT_GE(t, sfsCtimeOf(f), before);
    KT_LE(t, sfsCtimeOf(f), after);

    KT_FALSE(t, sfsChgrp(-1, 1000));
    KT_FALSE(t, sfsChgrp(9999, 1000));
}

/* Same real point as sfsPersistsChownedUidAcrossUnmountRemount above, one
   field over: proves the chgrp write path itself lands on disk. */
KTEST(sfsPersistsChgrpedGidAcrossUnmountRemount)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("chgrppersist.txt");
    if (!KT_TRUE(t, f > 0))
        return;
    KT_TRUE(t, sfsChgrp(f, 7124));

    sfsUnmount();
    KT_TRUE(t, sfsMount());

    int32 reloaded = sfsLookup("chgrppersist.txt");
    KT_EQ(t, reloaded, f);
    KT_EQU(t, sfsGidOf(reloaded), 7124u);
}

/* Real directory-write enforcement at the on-disk choke point (sfs.c's
   createNode, shared by sfsCreateFile/sfsCreateDir/sfsCreateFifo/
   sfsCreateSymlink) -- mirrors coreTest.c's tmpfs
   coreVfsMkdirAndCreateRequireParentWrite for the SFS backing store.
   ktestRunAll always runs as uid 0, and vfsPermitted has no uid-0 bypass,
   so removing the parent's own owner-write bit denies even its own
   creator/owner. */
KTEST(sfsCreateNodeRequiresParentWrite)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateDir("dwparent") > 0);
    KT_TRUE(t, sfsChmod(sfsLookup("dwparent"), 0555));   /* r-x, no owner-write */

    KT_EQ(t, sfsCreateFile("dwparent/f"), -1);
    KT_EQ(t, sfsCreateDir("dwparent/sub"), -1);

    KT_TRUE(t, sfsChmod(sfsLookup("dwparent"), fsModeDefaultDir));
    KT_TRUE(t, sfsCreateFile("dwparent/f") > 0);
    KT_TRUE(t, sfsCreateDir("dwparent/sub") > 0);
}

/* Same parent-write rule applied to sfsLink -- adding a new hardlink NAME
   is a directory-entry creation exactly like sfsCreateFile, so it needs
   write permission on the PARENT of the new name, not any permission on
   the existing target being linked to. */
KTEST(sfsLinkRequiresParentWriteOnNewName)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateFile("lwtarget.txt") > 0);
    KT_TRUE(t, sfsCreateDir("lwparent") > 0);
    KT_TRUE(t, sfsChmod(sfsLookup("lwparent"), 0555));

    KT_EQ(t, sfsLink("lwtarget.txt", "lwparent/alias"), -1);

    KT_TRUE(t, sfsChmod(sfsLookup("lwparent"), fsModeDefaultDir));
    KT_TRUE(t, sfsLink("lwtarget.txt", "lwparent/alias") > 0);
}

/* Real unlink(2)/rmdir(2) semantics at the on-disk choke point (sfs.c's
   sfsRemove) -- mirrors coreTest.c's tmpfs
   coreVfsUnlinkAndRmdirRequireParentWrite: removing an entry needs write
   permission on the PARENT, never the entry's own mode. */
KTEST(sfsRemoveRequiresParentWrite)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateDir("rwparent") > 0);
    KT_TRUE(t, sfsCreateFile("rwparent/f") > 0);

    KT_TRUE(t, sfsChmod(sfsLookup("rwparent"), 0555));
    KT_FALSE(t, sfsRemove("rwparent/f"));

    KT_TRUE(t, sfsChmod(sfsLookup("rwparent"), fsModeDefaultDir));
    KT_TRUE(t, sfsRemove("rwparent/f"));
}

/* Real POSIX search permission at the on-disk choke point (sfs.c's
   lookupFrom, checked on every directory ANCESTOR a path walks through) --
   mirrors coreTest.c's tmpfs coreVfsLookupRequiresAncestorExec. Removing
   the exec bit on an ancestor makes every lookup THROUGH it fail even
   though the target inode itself never changed. */
KTEST(sfsLookupRequiresAncestorExec)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateDir("xancestor") > 0);
    int32 leaf = sfsCreateFile("xancestor/leaf");
    KT_TRUE(t, leaf > 0);

    KT_TRUE(t, sfsChmod(sfsLookup("xancestor"), 0600));   /* rw-, no exec */
    KT_EQ(t, sfsLookup("xancestor/leaf"), -1);

    KT_TRUE(t, sfsChmod(sfsLookup("xancestor"), fsModeDefaultDir));
    KT_EQ(t, sfsLookup("xancestor/leaf"), leaf);
}

KTEST(sfsSymlinkResolvesToTargetFile)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("real.txt");
    KT_TRUE(t, f > 0);
    const char* msg = "reached through a link";
    uint64 len = strlen(msg);
    KT_EQ(t, sfsWriteAt(f, 0, msg, len), (int64)len);

    int32 link = sfsCreateSymlink("link.txt", "real.txt");
    KT_TRUE(t, link > 0);
    KT_NE(t, link, f);

    /* sfsLookup follows the symlink: it must land on the SAME inode as
       looking the real file up directly, not a copy of it. */
    KT_EQ(t, sfsLookup("link.txt"), f);
    KT_EQ(t, sfsTypeOf(sfsLookup("link.txt")), sfsTypeFile);

    char back[64];
    memset(back, 0, sizeof(back));
    KT_EQ(t, sfsReadAt(sfsLookup("link.txt"), 0, back, len), (int64)len);
    KT_EQ(t, memcmp(back, msg, len), 0);
}

KTEST(sfsReadlinkReturnsStoredTargetAndNoFollowSeesTheLinkItself)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("real.txt");
    KT_TRUE(t, f > 0);
    int32 link = sfsCreateSymlink("link.txt", "real.txt");
    KT_TRUE(t, link > 0);

    char target[32];
    memset(target, 0, sizeof(target));
    KT_TRUE(t, sfsReadlink(link, target, sizeof(target)));
    KT_STREQ(t, target, "real.txt");

    /* No-follow gives back the symlink's own inode (and its own type),
       distinct from what sfsLookup (which follows) would give. */
    int32 noFollow = sfsLookupNoFollow("link.txt");
    KT_EQ(t, noFollow, link);
    KT_EQ(t, sfsTypeOf(noFollow), sfsTypeSymlink);
    KT_NE(t, noFollow, f);
}

KTEST(sfsReadlinkRejectsNonSymlinkAndUndersizedBuffer)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("plain.txt");
    KT_TRUE(t, f > 0);
    char buf[8];
    KT_FALSE(t, sfsReadlink(f, buf, sizeof(buf)));

    int32 link = sfsCreateSymlink("link.txt", "a target long enough to not fit");
    KT_TRUE(t, link > 0);

    char tiny[4];
    KT_FALSE(t, sfsReadlink(link, tiny, sizeof(tiny)));
}

KTEST(sfsSymlinkToDirectoryAllowsDescendingThroughIt)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateDir("realdir") > 0);
    int32 f = sfsCreateFile("realdir/inside.txt");
    KT_TRUE(t, f > 0);
    KT_EQ(t, sfsWriteAt(f, 0, "found me", 8), 8);

    int32 link = sfsCreateSymlink("linkdir", "realdir");
    KT_TRUE(t, link > 0);
    KT_EQ(t, sfsTypeOf(link), sfsTypeSymlink);

    /* The symlink is an INTERMEDIATE component here (there is more path
       after it), which must still be followed even though sfsLookup's
       "final component" special case does not apply. */
    int32 viaLink = sfsLookup("linkdir/inside.txt");
    KT_EQ(t, viaLink, f);

    /* Creating a new file through the symlinked directory must land in the
       real directory, reachable from both paths. */
    KT_TRUE(t, sfsCreateFile("linkdir/new.txt") > 0);
    KT_EQ(t, sfsLookup("realdir/new.txt"), sfsLookup("linkdir/new.txt"));
}

KTEST(sfsDanglingSymlinkFailsLookupButReadlinkStillWorks)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 link = sfsCreateSymlink("broken.txt", "does/not/exist.txt");
    KT_TRUE(t, link > 0);

    KT_EQ(t, sfsLookup("broken.txt"), -1);

    char target[64];
    memset(target, 0, sizeof(target));
    KT_TRUE(t, sfsReadlink(link, target, sizeof(target)));
    KT_STREQ(t, target, "does/not/exist.txt");
}

KTEST(sfsSymlinkCycleFailsCleanlyInsteadOfHanging)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateSymlink("a", "b") > 0);
    KT_TRUE(t, sfsCreateSymlink("b", "a") > 0);

    /* The whole point: this must return with an error, not recurse forever
       (or overflow the caller's stack) chasing a->b->a->b->... */
    KT_EQ(t, sfsLookup("a"), -1);
    KT_EQ(t, sfsLookup("b"), -1);

    /* The table must still be in a usable state afterward - a failed
       resolution is not the same thing as a corrupted filesystem. */
    KT_TRUE(t, sfsCreateFile("stillworks.txt") > 0);
}

KTEST(sfsCreateSymlinkRejectsEmptyOrOversizedTarget)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_EQ(t, sfsCreateSymlink("empty.txt", ""), -1);

    char huge[sfsSymlinkTargetMax + 2];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = 0;
    KT_EQ(t, sfsCreateSymlink("huge.txt", huge), -1);

    /* Rejected attempts must not leave a half-created inode or directory
       entry behind. */
    KT_EQ(t, sfsLookupNoFollow("empty.txt"), -1);
    KT_EQ(t, sfsLookupNoFollow("huge.txt"), -1);
}

KTEST(sfsRemoveSymlinkDoesNotAffectTarget)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("real.txt");
    KT_TRUE(t, f > 0);
    KT_EQ(t, sfsWriteAt(f, 0, "untouched", 9), 9);
    KT_TRUE(t, sfsCreateSymlink("link.txt", "real.txt") > 0);

    KT_TRUE(t, sfsRemove("link.txt"));
    KT_EQ(t, sfsLookupNoFollow("link.txt"), -1);

    /* The target must survive with its data intact - unlink of a symlink
       must never recurse into what it points at. */
    int32 stillThere = sfsLookup("real.txt");
    KT_EQ(t, stillThere, f);
    char back[16];
    memset(back, 0, sizeof(back));
    KT_EQ(t, sfsReadAt(stillThere, 0, back, 9), 9);
    KT_EQ(t, memcmp(back, "untouched", 9), 0);
}

KTEST(sfsSymlinkPersistsAcrossUnmountRemount)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("real.txt");
    KT_TRUE(t, f > 0);
    KT_EQ(t, sfsWriteAt(f, 0, "on disk for real", 17), 17);
    int32 link = sfsCreateSymlink("link.txt", "real.txt");
    KT_TRUE(t, link > 0);

    sfsUnmount();
    KT_TRUE(t, sfsMount());

    KT_EQ(t, sfsLookup("link.txt"), f);
    KT_EQ(t, sfsLookupNoFollow("link.txt"), link);
    KT_EQ(t, sfsTypeOf(link), sfsTypeSymlink);

    char target[32];
    memset(target, 0, sizeof(target));
    KT_TRUE(t, sfsReadlink(link, target, sizeof(target)));
    KT_STREQ(t, target, "real.txt");
}

KTEST(sfsFileGrowsThroughDoubleIndirectBlock)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("huge.bin");
    KT_TRUE(t, f > 0);

    /* sfsDirectBlocks (10) + sfsPtrsPerBlock (128) = 138 logical blocks are
       reachable through direct + single indirect addressing; block 138
       onward can only be reached through the double indirect block added
       in this wave. Starting a few blocks before that boundary and writing
       past it exercises the crossing and the double-indirect-only region
       in one write (sfsWriteAt zero-fills 0..134 first, all still within
       single-indirect range, before this pattern lands on 135..144). */
    #define sfsDoubleSpanBlocks 10
    #define sfsDoubleStartBlock 135
    #define sfsDoubleSpan (sfsDoubleSpanBlocks * sfsBlockSize)
    uint8* pattern = (uint8*)kmalloc(sfsDoubleSpan);
    uint8* verify = (uint8*)kmalloc(sfsDoubleSpan);
    KT_NOTNULL(t, pattern);
    KT_NOTNULL(t, verify);
    if (!pattern || !verify) {
        kfree(pattern);
        kfree(verify);
        return;
    }
    for (int i = 0; i < sfsDoubleSpan; i++)
        pattern[i] = (uint8)(i * 17 + 3);

    uint64 offset = (uint64)sfsDoubleStartBlock * sfsBlockSize;
    KT_EQ(t, sfsWriteAt(f, offset, pattern, sfsDoubleSpan), sfsDoubleSpan);
    KT_EQ(t, sfsSizeOf(f), offset + sfsDoubleSpan);

    memset(verify, 0, sfsDoubleSpan);
    KT_EQ(t, sfsReadAt(f, offset, verify, sfsDoubleSpan), sfsDoubleSpan);
    KT_EQ(t, memcmp(verify, pattern, sfsDoubleSpan), 0);

    /* Patch a window straddling the single-indirect/double-indirect
       boundary itself (logical block 137/138) and confirm only that slice
       changed -- the same "boundary patch" shape as
       sfsFileGrowsThroughIndirectBlock, one level deeper. */
    uint8 patch[48];
    memset(patch, 0xCC, sizeof(patch));
    uint64 patchOffset = 138 * sfsBlockSize - 24;
    KT_EQ(t, sfsWriteAt(f, patchOffset, patch, sizeof(patch)), (int64)sizeof(patch));

    KT_EQ(t, sfsReadAt(f, offset, verify, sfsDoubleSpan), sfsDoubleSpan);
    uint64 patchInSpan = patchOffset - offset;
    KT_EQ(t, memcmp(verify + patchInSpan, patch, sizeof(patch)), 0);
    KT_EQ(t, memcmp(verify, pattern, patchInSpan), 0);

    kfree(pattern);
    kfree(verify);
    #undef sfsDoubleSpan
    #undef sfsDoubleStartBlock
    #undef sfsDoubleSpanBlocks
}

KTEST(sfsTruncateFreesDoubleIndirectBlocks)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("hugeshrink.bin");
    KT_TRUE(t, f > 0);

    /* Baseline is taken AFTER creating the file, not before: creating the
       very first entry in a previously-empty root directory grows the
       root's own data block permanently (it is a directory entry, not file
       content, and stays even after truncating the FILE back to empty) --
       that is correct, unrelated bookkeeping this test must not conflate
       with whether truncating hugeshrink.bin's own content gives its
       blocks back. */
    uint64 baseline = sfsFreeDataBlocks();
    KT_GT(t, baseline, 0);

    #define sfsShrinkSpanBlocks 8
    #define sfsShrinkStartBlock 136
    #define sfsShrinkSpan (sfsShrinkSpanBlocks * sfsBlockSize)
    uint8* chunk = (uint8*)kmalloc(sfsShrinkSpan);
    KT_NOTNULL(t, chunk);
    if (!chunk)
        return;
    memset(chunk, 0x91, sfsShrinkSpan);
    uint64 offset = (uint64)sfsShrinkStartBlock * sfsBlockSize;
    KT_EQ(t, sfsWriteAt(f, offset, chunk, sfsShrinkSpan), sfsShrinkSpan);
    kfree(chunk);
    #undef sfsShrinkSpan
    #undef sfsShrinkStartBlock
    #undef sfsShrinkSpanBlocks

    /* This file now owns: 10 direct + 1 single-indirect holder + 128
       single-indirect data blocks + 1 double-indirect holder + 1 inner
       indirect holder + a handful of double-indirect data blocks. Every
       one of those must come back. */
    KT_LT(t, sfsFreeDataBlocks(), baseline);

    KT_TRUE(t, sfsTruncate(f, 0));
    KT_EQ(t, sfsSizeOf(f), 0);
    KT_EQU(t, sfsFreeDataBlocks(), baseline);

    /* And the inode itself must still be usable afterward, not left in some
       half-truncated state. */
    KT_EQ(t, sfsWriteAt(f, 0, "reused", 6), 6);
    KT_EQ(t, sfsSizeOf(f), 6);
}

KTEST(sfsFileGrowsThroughTripleIndirectBlock)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("huger.bin");
    KT_TRUE(t, f > 0);

    /* sfsDirectBlocks (10) + sfsPtrsPerBlock (128) + sfsPtrsPerBlock^2
       (16384) = 16522 logical blocks are reachable through direct + single
       + double indirect addressing; block 16522 onward can only be reached
       through the triple indirect block added in this wave. Physically
       reaching that boundary with real writes from offset 0 would mean
       sfsWriteAt zero-filling ~8 MiB of real leading blocks first (its own
       "no sparse holes" contract, see its doc comment) -- correct, but far
       too slow for a headless boot-time gate. sfsTruncate's own grow path
       is a genuine sparse hole instead (see sfsTruncateInode: growing only
       ever rewrites the size field, no block is touched), the same
       lseek-past-EOF-then-write shape a real filesystem's holes have -- so
       pre-growing with it reaches the boundary for free, in O(1), then a
       real write of a handful of blocks exercises the actual double/triple
       crossing without paying for the untouched blocks in between. */
    #define sfsTripleBoundaryBlock 16522
    #define sfsTripleSpanBlocks 12
    #define sfsTripleStartBlock (sfsTripleBoundaryBlock - 6)
    #define sfsTripleSpan (sfsTripleSpanBlocks * sfsBlockSize)
    uint64 startOffset = (uint64)sfsTripleStartBlock * sfsBlockSize;
    KT_TRUE(t, sfsTruncate(f, startOffset));
    KT_EQ(t, sfsSizeOf(f), startOffset);

    uint8* pattern = (uint8*)kmalloc(sfsTripleSpan);
    uint8* verify = (uint8*)kmalloc(sfsTripleSpan);
    KT_NOTNULL(t, pattern);
    KT_NOTNULL(t, verify);
    if (!pattern || !verify) {
        kfree(pattern);
        kfree(verify);
        return;
    }
    for (int i = 0; i < sfsTripleSpan; i++)
        pattern[i] = (uint8)(i * 29 + 7);

    /* Six blocks land in the double-indirect region (16516..16521), the
       other six only exist through the new triple-indirect block
       (16522..16527) -- one write straddles both. */
    KT_EQ(t, sfsWriteAt(f, startOffset, pattern, sfsTripleSpan), sfsTripleSpan);
    KT_EQ(t, sfsSizeOf(f), startOffset + sfsTripleSpan);

    memset(verify, 0, sfsTripleSpan);
    KT_EQ(t, sfsReadAt(f, startOffset, verify, sfsTripleSpan), sfsTripleSpan);
    KT_EQ(t, memcmp(verify, pattern, sfsTripleSpan), 0);

    /* Patch a window straddling the double-indirect/triple-indirect
       boundary itself (logical block 16521/16522) -- same "boundary patch"
       shape as sfsFileGrowsThroughDoubleIndirectBlock, one level deeper. */
    uint8 patch[48];
    memset(patch, 0xAB, sizeof(patch));
    uint64 patchOffset = (uint64)sfsTripleBoundaryBlock * sfsBlockSize - 24;
    KT_EQ(t, sfsWriteAt(f, patchOffset, patch, sizeof(patch)), (int64)sizeof(patch));

    KT_EQ(t, sfsReadAt(f, startOffset, verify, sfsTripleSpan), sfsTripleSpan);
    uint64 patchInSpan = patchOffset - startOffset;
    KT_EQ(t, memcmp(verify + patchInSpan, patch, sizeof(patch)), 0);
    KT_EQ(t, memcmp(verify, pattern, patchInSpan), 0);

    kfree(pattern);
    kfree(verify);
    #undef sfsTripleSpan
    #undef sfsTripleStartBlock
    #undef sfsTripleSpanBlocks
    #undef sfsTripleBoundaryBlock
}

KTEST(sfsTruncateFreesTripleIndirectBlocks)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("hugershrink.bin");
    KT_TRUE(t, f > 0);

    /* Baseline taken after creating the file, same reasoning as
       sfsTruncateFreesDoubleIndirectBlocks: the root directory's own data
       block is unrelated bookkeeping this test must not conflate with
       hugershrink.bin's own content. */
    uint64 baseline = sfsFreeDataBlocks();
    KT_GT(t, baseline, 0);

    /* Grow straight to the triple-indirect boundary via sfsTruncate (O(1),
       a genuine sparse hole -- see sfsFileGrowsThroughTripleIndirectBlock
       above) and write ENTIRELY inside the triple-indirect region, never
       touching a single/double-indirect block. That keeps inode->indirect
       and inode->doubleIndirect at 0, so the shrink-to-0 below only ever
       does a cheap in-memory "still unallocated" check across the whole
       single+double logical range instead of re-reading real holder blocks
       for it -- proportional to the actual work, not to how far into the
       address space the file happens to reach. */
    #define sfsTripleBoundaryBlock 16522
    #define sfsShrink2SpanBlocks 8
    #define sfsShrink2Span (sfsShrink2SpanBlocks * sfsBlockSize)
    uint64 offset = (uint64)sfsTripleBoundaryBlock * sfsBlockSize;
    KT_TRUE(t, sfsTruncate(f, offset));

    uint8* chunk = (uint8*)kmalloc(sfsShrink2Span);
    KT_NOTNULL(t, chunk);
    if (!chunk)
        return;
    memset(chunk, 0x62, sfsShrink2Span);
    KT_EQ(t, sfsWriteAt(f, offset, chunk, sfsShrink2Span), sfsShrink2Span);
    kfree(chunk);
    #undef sfsShrink2Span
    #undef sfsShrink2SpanBlocks
    #undef sfsTripleBoundaryBlock

    /* This file now owns: 1 triple-indirect holder + 1 mid holder + 1
       leaf-pointer holder + a handful of triple-indirect data blocks.
       Every one of those must come back. */
    KT_LT(t, sfsFreeDataBlocks(), baseline);

    KT_TRUE(t, sfsTruncate(f, 0));
    KT_EQ(t, sfsSizeOf(f), 0);
    KT_EQU(t, sfsFreeDataBlocks(), baseline);

    /* And the inode itself must still be usable afterward. */
    KT_EQ(t, sfsWriteAt(f, 0, "reused", 6), 6);
    KT_EQ(t, sfsSizeOf(f), 6);
}

KTEST(sfsHardlinkSharesInodeAndData)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("original.txt");
    KT_TRUE(t, f > 0);
    const char* msg = "shared bytes";
    uint64 len = strlen(msg);
    KT_EQ(t, sfsWriteAt(f, 0, msg, len), (int64)len);
    KT_EQ(t, sfsLinkCountOf(f), 1);

    int32 linked = sfsLink("original.txt", "alias.txt");
    KT_EQ(t, linked, f);
    KT_EQ(t, sfsLinkCountOf(f), 2);
    KT_EQ(t, sfsLookup("alias.txt"), f);

    /* A write through the alias must be visible through the original name:
       they are not two files that happen to match, they are the same
       inode. */
    KT_EQ(t, sfsWriteAt(sfsLookup("alias.txt"), len, " too", 4), 4);
    char back[32];
    memset(back, 0, sizeof(back));
    KT_EQ(t, sfsReadAt(f, 0, back, len + 4), (int64)(len + 4));
    KT_EQ(t, memcmp(back, "shared bytes too", len + 4), 0);
}

KTEST(sfsHardlinkRejectsDirectoriesAndBadArguments)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateDir("adir") > 0);
    KT_EQ(t, sfsLink("adir", "adiralias"), -1);

    int32 f = sfsCreateFile("real.txt");
    KT_TRUE(t, f > 0);
    KT_EQ(t, sfsLink("does/not/exist.txt", "alias.txt"), -1);

    KT_TRUE(t, sfsCreateFile("taken.txt") > 0);
    KT_EQ(t, sfsLink("real.txt", "taken.txt"), -1);

    /* None of the rejected attempts may have left a stray link count or
       directory entry behind. */
    KT_EQ(t, sfsLinkCountOf(f), 1);
    KT_EQ(t, sfsLookup("alias.txt"), -1);
}

KTEST(sfsRemoveOneHardlinkKeepsOtherAlive)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("first.txt");
    KT_TRUE(t, f > 0);
    KT_EQ(t, sfsWriteAt(f, 0, "durable", 7), 7);
    KT_TRUE(t, sfsLink("first.txt", "second.txt") > 0);

    KT_TRUE(t, sfsRemove("first.txt"));
    KT_EQ(t, sfsLookup("first.txt"), -1);

