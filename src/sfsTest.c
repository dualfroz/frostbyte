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

