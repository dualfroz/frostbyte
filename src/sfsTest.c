#include "ktest.h"
#include "sfs.h"
#include "ata.h"
#include "string.h"
#include "heap.h"
#include "console.h"
#include "rtc.h"
#include "fs.h"

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

KTEST(sfsCreateNodeRequiresParentWrite)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateDir("dwparent") > 0);
    KT_TRUE(t, sfsChmod(sfsLookup("dwparent"), 0555));

    KT_EQ(t, sfsCreateFile("dwparent/f"), -1);
    KT_EQ(t, sfsCreateDir("dwparent/sub"), -1);

    KT_TRUE(t, sfsChmod(sfsLookup("dwparent"), fsModeDefaultDir));
    KT_TRUE(t, sfsCreateFile("dwparent/f") > 0);
    KT_TRUE(t, sfsCreateDir("dwparent/sub") > 0);
}

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

KTEST(sfsLookupRequiresAncestorExec)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    KT_TRUE(t, sfsCreateDir("xancestor") > 0);
    int32 leaf = sfsCreateFile("xancestor/leaf");
    KT_TRUE(t, leaf > 0);

    KT_TRUE(t, sfsChmod(sfsLookup("xancestor"), 0600));
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

    int32 viaLink = sfsLookup("linkdir/inside.txt");
    KT_EQ(t, viaLink, f);

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

    KT_EQ(t, sfsLookup("a"), -1);
    KT_EQ(t, sfsLookup("b"), -1);

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

    KT_LT(t, sfsFreeDataBlocks(), baseline);

    KT_TRUE(t, sfsTruncate(f, 0));
    KT_EQ(t, sfsSizeOf(f), 0);
    KT_EQU(t, sfsFreeDataBlocks(), baseline);

    KT_EQ(t, sfsWriteAt(f, 0, "reused", 6), 6);
    KT_EQ(t, sfsSizeOf(f), 6);
}

KTEST(sfsFileGrowsThroughTripleIndirectBlock)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("huger.bin");
    KT_TRUE(t, f > 0);

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

    KT_EQ(t, sfsWriteAt(f, startOffset, pattern, sfsTripleSpan), sfsTripleSpan);
    KT_EQ(t, sfsSizeOf(f), startOffset + sfsTripleSpan);

    memset(verify, 0, sfsTripleSpan);
    KT_EQ(t, sfsReadAt(f, startOffset, verify, sfsTripleSpan), sfsTripleSpan);
    KT_EQ(t, memcmp(verify, pattern, sfsTripleSpan), 0);

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

    uint64 baseline = sfsFreeDataBlocks();
    KT_GT(t, baseline, 0);

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

    KT_LT(t, sfsFreeDataBlocks(), baseline);

    KT_TRUE(t, sfsTruncate(f, 0));
    KT_EQ(t, sfsSizeOf(f), 0);
    KT_EQU(t, sfsFreeDataBlocks(), baseline);

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

    int32 viaSecond = sfsLookup("second.txt");
    KT_EQ(t, viaSecond, f);
    KT_EQ(t, sfsLinkCountOf(viaSecond), 1);

    char back[8];
    KT_EQ(t, sfsReadAt(viaSecond, 0, back, 7), 7);
    KT_EQ(t, memcmp(back, "durable", 7), 0);
}

KTEST(sfsRemoveLastHardlinkFreesInodeAndBlocks)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    uint64 baseline = sfsFreeDataBlocks();

    int32 f = sfsCreateFile("only.txt");
    KT_TRUE(t, f > 0);
    KT_EQ(t, sfsWriteAt(f, 0, "temporary", 9), 9);
    KT_TRUE(t, sfsLink("only.txt", "also.txt") > 0);
    KT_LT(t, sfsFreeDataBlocks(), baseline);

    KT_TRUE(t, sfsRemove("only.txt"));
    KT_NE(t, sfsLookup("also.txt"), -1);
    KT_TRUE(t, sfsRemove("also.txt"));
    KT_EQ(t, sfsLookup("also.txt"), -1);

    KT_EQU(t, sfsFreeDataBlocks(), baseline);

    int32 again = sfsCreateFile("only.txt");
    KT_TRUE(t, again > 0);
    KT_EQ(t, sfsSizeOf(again), 0);
}

KTEST(sfsHardlinkThroughSymlinkTargetsRealInode)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("real.txt");
    KT_TRUE(t, f > 0);
    KT_TRUE(t, sfsCreateSymlink("link.txt", "real.txt") > 0);

    int32 linked = sfsLink("link.txt", "alias.txt");
    KT_EQ(t, linked, f);
    KT_EQ(t, sfsLinkCountOf(f), 2);
    KT_EQ(t, sfsTypeOf(sfsLookup("alias.txt")), sfsTypeFile);
}

KTEST(sfsHardlinkPersistsAcrossUnmountRemount)
{
    if (!KT_TRUE(t, sfsFormat()))
        return;

    int32 f = sfsCreateFile("orig.txt");
    KT_TRUE(t, f > 0);
    KT_EQ(t, sfsWriteAt(f, 0, "persisted", 9), 9);
    KT_TRUE(t, sfsLink("orig.txt", "alias.txt") > 0);

    sfsUnmount();
    KT_TRUE(t, sfsMount());

    int32 viaOrig = sfsLookup("orig.txt");
    int32 viaAlias = sfsLookup("alias.txt");
    KT_EQ(t, viaOrig, f);
    KT_EQ(t, viaOrig, viaAlias);
    KT_EQ(t, sfsLinkCountOf(f), 2);

    char back[16];
    memset(back, 0, sizeof(back));
    KT_EQ(t, sfsReadAt(viaAlias, 0, back, 9), 9);
    KT_EQ(t, memcmp(back, "persisted", 9), 0);
}

KTEST(sfsFormatLeavesCleanStateForLaterUse)
{
    KT_TRUE(t, sfsFormat());
    KT_EQ(t, sfsLookup(""), sfsRootInode);

    char name[sfsNameMax + 1];
    uint32 type;
    KT_FALSE(t, sfsReaddirAt(sfsRootInode, 0, name, sizeof(name), &type));
}
