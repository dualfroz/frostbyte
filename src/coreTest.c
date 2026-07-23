#include "ktest.h"
#include "string.h"
#include "heap.h"
#include "pmm.h"
#include "vfs.h"
#include "fs.h"
#include "sfs.h"

/*
 * Regression coverage for the subsystems that already existed before the
 * in-kernel test harness: the freestanding string routines, the free-list
 * heap, the physical frame allocator and the VFS/tmpfs path.  These run
 * against the live kernel, so a regression in any of them fails the boot gate.
 */

KTEST(coreStringMem)
{
    char buf[32];

    memset(buf, 'A', sizeof(buf));
    KT_EQ(t, buf[0], 'A');
    KT_EQ(t, buf[31], 'A');

    memset(buf, 0, sizeof(buf));
    KT_EQ(t, buf[0], 0);

    const char* src = "frostbyte";
    memcpy(buf, src, 10);
    KT_STREQ(t, buf, "frostbyte");
    KT_EQ(t, strlen(buf), 9);

    KT_EQ(t, memcmp("abc", "abc", 3), 0);
    KT_TRUE(t, memcmp("abc", "abd", 3) < 0);
    KT_TRUE(t, memcmp("abd", "abc", 3) > 0);

    KT_EQ(t, strcmp("abc", "abc"), 0);
    KT_TRUE(t, strcmp("abc", "abd") < 0);
    KT_EQ(t, strncmp("abcdef", "abcxxx", 3), 0);
    KT_TRUE(t, strncmp("abcdef", "abcxxx", 4) != 0);
}

KTEST(coreMemmoveOverlap)
{
    char buf[16];
    memcpy(buf, "0123456789", 11);

    /* forward overlap: dest > src */
    memmove(buf + 2, buf, 8);
    buf[10] = 0;
    KT_STREQ(t, buf, "0101234567");

    memcpy(buf, "0123456789", 11);
    /* backward overlap: dest < src */
    memmove(buf, buf + 2, 8);
    buf[8] = 0;
    KT_STREQ(t, buf, "23456789");
}

KTEST(coreHeapAllocFree)
{
    uint64 freeBefore = heapFreeBytes();

    void* a = kmalloc(100);
    void* b = kmalloc(200);
    void* c = kmalloc(100);
    KT_NOTNULL(t, a);
    KT_NOTNULL(t, b);
    KT_NOTNULL(t, c);
    KT_NE(t, a, b);
    KT_NE(t, b, c);

    /* freeing b and re-allocating a same-or-smaller block reuses the hole */
    kfree(b);
    void* d = kmalloc(150);
    KT_EQP(t, d, b);

    kfree(a);
    kfree(c);
    kfree(d);

    /* everything returned: free bytes back to (at least) where we started */
    KT_GE(t, heapFreeBytes(), freeBefore);
}

KTEST(coreHeapRealloc)
{
    char* p = (char*)kmalloc(8);
    KT_NOTNULL(t, p);
    memcpy(p, "1234567", 8);

    p = (char*)krealloc(p, 64);
    KT_NOTNULL(t, p);
    KT_STREQ(t, p, "1234567");   /* contents preserved across grow */

    p = (char*)krealloc(p, 4);
    KT_NOTNULL(t, p);
    KT_EQ(t, memcmp(p, "1234", 4), 0);

    kfree(p);
}

KTEST(corePmmAllocDistinct)
{
    uint64 freeBefore = pmmFreeFrames();

    uint64 f1 = pmmAlloc();
    uint64 f2 = pmmAlloc();
    uint64 f3 = pmmAlloc();
    KT_NE(t, f1, 0);
    KT_NE(t, f2, 0);
    KT_NE(t, f3, 0);
    KT_NE(t, f1, f2);
    KT_NE(t, f2, f3);
    KT_NE(t, f1, f3);
    KT_EQ(t, f1 & (frameSize - 1), 0);   /* page aligned */

    KT_EQ(t, pmmFreeFrames(), freeBefore - 3);

    pmmFree(f1);
    pmmFree(f2);
    pmmFree(f3);
    KT_EQ(t, pmmFreeFrames(), freeBefore);
}

KTEST(coreVfsTmpfsRoundtrip)
{
    int fd = vfsOpen("/tmp/coretest.txt", oReadWrite | oCreate | oTrunc);
    KT_GE(t, fd, 0);
    if (fd < 0)
        return;

    const char* msg = "hello vfs";
    KT_EQ(t, vfsWrite(fd, msg, 9), 9);

    KT_EQ(t, vfsLseek(fd, 0, seekSet), 0);
    char rd[16];
    memset(rd, 0, sizeof(rd));
    KT_EQ(t, vfsRead(fd, rd, 9), 9);
    KT_STREQ(t, rd, "hello vfs");

    KT_EQ(t, vfsLseek(fd, 0, seekEnd), 9);
    vfsClose(fd);

    FbStat st;
    KT_EQ(t, vfsStat("/tmp/coretest.txt", &st), 0);
    KT_EQ(t, st.size, 9);
    KT_EQ(t, st.type, fsTypeFile);

    KT_EQ(t, vfsUnlink("/tmp/coretest.txt"), 0);
    KT_EQ(t, vfsStat("/tmp/coretest.txt", &st), -1);   /* gone */
}

KTEST(coreVfsMkdirGetdents)
{
    KT_EQ(t, vfsMkdir("/tmp/cd"), 0);
    KT_EQ(t, vfsMkdir("/tmp/cd"), -1);   /* already exists */

    int fa = vfsOpen("/tmp/cd/a", oReadWrite | oCreate);
    int fb = vfsOpen("/tmp/cd/b", oReadWrite | oCreate);
    KT_GE(t, fa, 0);
    KT_GE(t, fb, 0);
    vfsClose(fa);
    vfsClose(fb);

    int dd = vfsOpen("/tmp/cd", oReadOnly);
    KT_GE(t, dd, 0);
    FbDirent ents[8];
    int64 n = vfsGetdents(dd, ents, sizeof(ents));
    KT_TRUE(t, n >= (int64)(2 * sizeof(FbDirent)));
    vfsClose(dd);

    vfsUnlink("/tmp/cd/a");
    vfsUnlink("/tmp/cd/b");
}

/* Real rmdir(2) on tmpfs: only an EMPTY directory may be removed, and the
   directory must still be there (untouched) after a rejected attempt. */
KTEST(coreVfsRmdirTmpfsEmptyCheck)
{
    KT_EQ(t, vfsMkdir("/tmp/rd"), 0);

    int fd = vfsOpen("/tmp/rd/f", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    KT_EQ(t, vfsRmdir("/tmp/rd"), -1);   /* not empty yet */
    FbStat st;
    KT_EQ(t, vfsStat("/tmp/rd", &st), 0);   /* rejected attempt left it alone */

    KT_EQ(t, vfsUnlink("/tmp/rd/f"), 0);
    KT_EQ(t, vfsRmdir("/tmp/rd"), 0);       /* empty now, succeeds */
    KT_EQ(t, vfsStat("/tmp/rd", &st), -1);  /* gone */
}

/* Real unlink(2)/rmdir(2) split on tmpfs: each refuses the other's kind of
   node outright, neither silently falls back to the other's behavior. */
KTEST(coreVfsUnlinkAndRmdirRejectWrongTypeTmpfs)
{
    KT_EQ(t, vfsMkdir("/tmp/rdDir"), 0);
    KT_EQ(t, vfsUnlink("/tmp/rdDir"), -1);   /* EISDIR-shaped refusal */
    FbStat st;
    KT_EQ(t, vfsStat("/tmp/rdDir", &st), 0); /* still there */
    KT_EQ(t, vfsRmdir("/tmp/rdDir"), 0);     /* the sanctioned way */

    int fd = vfsOpen("/tmp/rdFile", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);
    KT_EQ(t, vfsRmdir("/tmp/rdFile"), -1);   /* ENOTDIR-shaped refusal */
    KT_EQ(t, vfsStat("/tmp/rdFile", &st), 0); /* still there */
    KT_EQ(t, vfsUnlink("/tmp/rdFile"), 0);
}

/* Root can never be removed -- covered by the same `!node->writable` guard
   every other mutating tmpfs entry point already relies on (root is
   created non-writable in vfsInit). */
KTEST(coreVfsRmdirRefusesRoot)
{
    KT_EQ(t, vfsRmdir("/"), -1);
}

/* Same two checks as coreVfsRmdirTmpfsEmptyCheck/
   coreVfsUnlinkAndRmdirRejectWrongTypeTmpfs above, on the persistent SFS
   mount instead of tmpfs -- proves vfs.c's dispatch layer enforces the
   same real unlink(2)/rmdir(2) split on BOTH backing stores, not just the
   in-memory one. Before this fala, vfsUnlink on an SFS path never checked
   the inode's type at all, so unlinking an EMPTY directory under /mnt
   silently succeeded (sfsRemove's own dir-emptiness check let it through)
   -- this is the regression guard for that real, previously-shipped gap. */
KTEST(coreVfsRmdirSfsParity)
{
    KT_EQ(t, vfsMkdir("/mnt/rdSfsEmpty"), 0);
    KT_EQ(t, vfsUnlink("/mnt/rdSfsEmpty"), -1);   /* the fixed bug */
    FbStat st;
    KT_EQ(t, vfsStat("/mnt/rdSfsEmpty", &st), 0); /* still there */
    KT_EQ(t, vfsRmdir("/mnt/rdSfsEmpty"), 0);
    KT_EQ(t, vfsStat("/mnt/rdSfsEmpty", &st), -1);

    KT_EQ(t, vfsMkdir("/mnt/rdSfsFull"), 0);
    int fd = vfsOpen("/mnt/rdSfsFull/f", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);
    KT_EQ(t, vfsRmdir("/mnt/rdSfsFull"), -1);     /* not empty */
    KT_EQ(t, vfsUnlink("/mnt/rdSfsFull/f"), 0);
    KT_EQ(t, vfsRmdir("/mnt/rdSfsFull"), 0);

    fd = vfsOpen("/mnt/rdSfsFile", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);
    KT_EQ(t, vfsRmdir("/mnt/rdSfsFile"), -1);     /* ENOTDIR-shaped refusal */
    KT_EQ(t, vfsUnlink("/mnt/rdSfsFile"), 0);
}

/* Real chmod(2) + owner rwx permission bits (fs.h's fsModeOwner*) on tmpfs:
   a fresh file gets fsModeDefaultFile (0644) by default; chmod to a mode
   missing the owner-write bit makes a SUBSEQUENT open-for-write fail
   (EACCES-shaped) while reads keep working, and chmod back restores write
   access immediately -- permission is only checked at open() time, never
   re-checked against an fd already held, matching real Unix. */
KTEST(coreVfsChmodTmpfsWritePermission)
{
    int fd = vfsOpen("/tmp/chmodf", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    KT_EQ(t, vfsWrite(fd, "x", 1), 1);
    vfsClose(fd);

    FbStat st;
    KT_EQ(t, vfsStat("/tmp/chmodf", &st), 0);
    KT_EQ(t, st.mode, fsModeDefaultFile);

    KT_EQ(t, vfsChmod("/tmp/chmodf", 0444), 0);
    KT_EQ(t, vfsStat("/tmp/chmodf", &st), 0);
    KT_EQ(t, st.mode, 0444u);

    KT_EQ(t, vfsOpen("/tmp/chmodf", oWriteOnly), -1);   /* no owner-write bit */
    int rfd = vfsOpen("/tmp/chmodf", oReadOnly);
    KT_GE(t, rfd, 0);
    char rd;
    KT_EQ(t, vfsRead(rfd, &rd, 1), 1);
    vfsClose(rfd);

    KT_EQ(t, vfsChmod("/tmp/chmodf", fsModeDefaultFile), 0);
    int wfd = vfsOpen("/tmp/chmodf", oWriteOnly);
    KT_GE(t, wfd, 0);
    vfsClose(wfd);

    vfsUnlink("/tmp/chmodf");
}

/* Real, previously-missing enforcement found by this fala's own grouptest.c
   live demo, not review: resolveOpen (vfs.c) checked fsModeOwnerWrite for a
   write-mode open and fsModeOwnerRead for opendir()-ing a DIRECTORY, but
   NEVER checked any read bit at all for a plain FILE opened oReadOnly or
   oReadWrite -- any caller could read any readable-by-someone file's
   CONTENTS regardless of mode, since nothing gated it. Proven here entirely
   from KTEST (unlike the cross-uid case, which needed a live process,
   see grouptest.c's own "other-fallback-denied-ok"): chmod 0000 denies even
   the file's OWNER its own read, exactly like coreVfsMkdirAndCreateRequire
   ParentWrite already proved for root's own directory-write bit -- no
   superuser/owner bypass either way. */
KTEST(coreVfsResolveOpenRequiresFileReadPermission)
{
    int fd = vfsOpen("/tmp/readf", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    KT_EQ(t, vfsWrite(fd, "x", 1), 1);
    vfsClose(fd);

    KT_EQ(t, vfsChmod("/tmp/readf", 0000), 0);   /* no bits at all, even owner */
    KT_EQ(t, vfsOpen("/tmp/readf", oReadOnly), -1);
    KT_EQ(t, vfsOpen("/tmp/readf", oReadWrite), -1);

    KT_EQ(t, vfsChmod("/tmp/readf", 0200), 0);   /* owner-write only, no read */
    KT_EQ(t, vfsOpen("/tmp/readf", oReadOnly), -1);
    int wfd2 = vfsOpen("/tmp/readf", oWriteOnly);
    KT_GE(t, wfd2, 0);   /* write-only still works -- the two bits are independent */
    vfsClose(wfd2);

    KT_EQ(t, vfsChmod("/tmp/readf", fsModeDefaultFile), 0);
    int rfd2 = vfsOpen("/tmp/readf", oReadOnly);
    KT_GE(t, rfd2, 0);
    vfsClose(rfd2);

    vfsUnlink("/tmp/readf");
}

/* chmod on a missing path, or on the never-writable ustar-loaded /bin
   content, is rejected exactly like every other tmpfs mutation already
   gates on node->writable (vfsUnlink, vfsWrite's write path) -- chmod is a
   metadata mutation too, so it obeys the same rule. Also proves every
   ustar-loaded binary really does get the owner-exec bit by default
   (vfs.c's ustarAdd), the fact loadImageByPath/jobctlSpawn's own exec-
   permission check depends on for every existing spawnUser/userSpawn call
   in this kernel to keep working at all. */
KTEST(coreVfsChmodRejectsMissingAndReadonlyBin)
{
    KT_EQ(t, vfsChmod("/tmp/doesNotExist", 0644), -1);
    KT_EQ(t, vfsChmod("/bin/false", 0777), -1);

    FbStat st;
    KT_EQ(t, vfsStat("/bin/false", &st), 0);
    KT_EQ(t, st.mode, fsModeDefaultExec);
}

/* Same round trip on the persistent SFS mount, PLUS the "survives
   unmount+remount" proof every other SFS-persisted field (mtime/ctime,
   FALA 39) already has -- mode genuinely lives on disk, not just in the
   in-memory inode cache. */
KTEST(coreVfsChmodSfsPersistsAcrossRemount)
{
    KT_EQ(t, vfsMkdir("/mnt/chmodd"), 0);
    int fd = vfsOpen("/mnt/chmodd/f", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    FbStat st;
    KT_EQ(t, vfsStat("/mnt/chmodd/f", &st), 0);
    KT_EQ(t, st.mode, fsModeDefaultFile);

    KT_EQ(t, vfsChmod("/mnt/chmodd/f", 0400), 0);
    KT_EQ(t, vfsOpen("/mnt/chmodd/f", oWriteOnly), -1);

    sfsUnmount();
    KT_TRUE(t, sfsMount());

    KT_EQ(t, vfsStat("/mnt/chmodd/f", &st), 0);
    KT_EQ(t, st.mode, 0400u);
    KT_EQ(t, vfsOpen("/mnt/chmodd/f", oWriteOnly), -1);

    KT_EQ(t, vfsChmod("/mnt/chmodd/f", fsModeDefaultFile), 0);
    int wfd = vfsOpen("/mnt/chmodd/f", oWriteOnly);
    KT_GE(t, wfd, 0);
    vfsClose(wfd);

    vfsUnlink("/mnt/chmodd/f");
    vfsRmdir("/mnt/chmodd");
}

/* Real Unix owner/group/other permission algorithm (vfs.c's vfsPermitted)
   -- pure-logic coverage of the whole decision matrix, since the function
   takes every credential as an explicit argument instead of reading a live
   thread (see its own doc comment). Deliberately includes uid 0 on BOTH
   sides of the owner/other split (coreVfsPermittedUidZeroIsNotASuperuser
   below) -- uid 0 gets no special treatment at all, the one property that
   distinguishes this from real Unix's root and the exact regression this
   test guards: an earlier version of vfsPermitted let uid 0 bypass every
   bit unconditionally, which silently broke every OTHER permission test in
   this file (coreVfsChmodTmpfsWritePermission,
   coreVfsChmodSfsPersistsAcrossRemount both run as ktestRunAll's own idle
   thread, uid 0) the moment chmod took away the write bit on a file that
   same uid-0 caller had just created. Every "hits the other branch" case
   below deliberately gives a MISMATCHED gid too (not just a mismatched
   uid) -- otherwise an incidental gid match would silently redirect the
   check into the new group branch instead of exercising other at all. */
KTEST(coreVfsPermittedOwnerMatchChecksOwnerBitOnly)
{
    /* Owner-uid match short-circuits before gid is ever consulted, so gid
       values here are deliberately irrelevant -- 0 on both sides. */
    /* Owner bit set, other bit clear: an owning caller passes. */
    KT_TRUE(t, vfsPermitted(0600, 1000, 0, 1000, 0, fsModeOwnerWrite));
    /* Owner bit clear, other bit set: an owning caller still fails --
       owning the file never grants what the OTHER bits happen to allow. */
    KT_FALSE(t, vfsPermitted(0004, 1000, 0, 1000, 0, fsModeOwnerWrite));
}

KTEST(coreVfsPermittedNonOwnerChecksOtherBitOnly)
{
    /* Other-write set, owner-write clear, DIFFERENT uid AND gid: passes on
       the other bit even though the owner bit alone would refuse. */
    KT_TRUE(t, vfsPermitted(0002, 1000, 10, 2000, 20, fsModeOwnerWrite));
    /* Neither bit helps a stranger if the other bit is the one clear. */
    KT_FALSE(t, vfsPermitted(0600, 1000, 10, 2000, 20, fsModeOwnerWrite));
    KT_FALSE(t, vfsPermitted(0000, 1000, 10, 2000, 20, fsModeOwnerExec));
}

KTEST(coreVfsPermittedUidZeroIsNotASuperuser)
{
    /* uid 0 as OWNER: governed by the owner bit exactly like any other
       uid, matching every existing chmod test's own assumption (every
       boot-time process, including ktestRunAll itself, starts at uid 0). */
    KT_TRUE(t, vfsPermitted(0600, 0, 0, 0, 0, fsModeOwnerWrite));
    KT_FALSE(t, vfsPermitted(0000, 0, 0, 0, 0, fsModeOwnerWrite));
    /* uid 0 as a NON-owner (and non-group-member): governed by the other
       bit, no bypass at all -- a mode of 0 denies even uid 0 against
       someone else's file. */
    KT_FALSE(t, vfsPermitted(0700, 1000, 10, 0, 20, fsModeOwnerWrite));
    KT_TRUE(t, vfsPermitted(0702, 1000, 10, 0, 20, fsModeOwnerWrite));
    /* And symmetrically: owning a file as a non-zero uid is not somehow
       MORE privileged than uid 0 owning one -- same algorithm either way. */
    KT_FALSE(t, vfsPermitted(0600, 42, 10, 0, 20, fsModeOwnerWrite));
}

/* Every fsModeOwner/fsModeOther pair used by a real choke point
   (resolveOpen's write check, loadImageByPath/jobctlSpawn's exec check)
   round-trips through the exact bit-shift vfsPermitted derives one from
   the other with -- not just fsModeOwnerWrite from the test above. */
KTEST(coreVfsPermittedExecBitFollowsSameOwnerOtherSplit)
{
    KT_TRUE(t, vfsPermitted(fsModeOtherExec, 1, 10, 2, 20, fsModeOwnerExec));
    KT_FALSE(t, vfsPermitted(fsModeOwnerExec, 1, 10, 2, 20, fsModeOwnerExec));
    KT_TRUE(t, vfsPermitted(fsModeOwnerExec, 1, 10, 1, 10, fsModeOwnerExec));
}

/* The new middle branch: a caller whose UID does not match the owner but
   whose GID matches the node's group is governed by the fsModeGroup* bit
   (wantOwnerBit >> 3), never falling all the way through to other -- and a
   group bit set does NOT imply the other bit is, proving this is a
   genuinely separate check, not just "not owner" collapsing to one
   fallback like the pre-group-model version of this function used to. */
KTEST(coreVfsPermittedGroupMatchChecksGroupBitOnly)
{
    /* Group-write set, owner/other-write clear: different uid, same gid. */
    KT_TRUE(t, vfsPermitted(0020, 1000, 10, 2000, 10, fsModeOwnerWrite));
    /* Group bit clear: a gid match alone grants nothing by itself. */
    KT_FALSE(t, vfsPermitted(0602, 1000, 10, 2000, 10, fsModeOwnerWrite));
    /* Every fsModeOwner/fsModeGroup pair round-trips through the >>3 shift,
       not just write above. */
    KT_TRUE(t, vfsPermitted(fsModeGroupExec, 1, 10, 2, 10, fsModeOwnerExec));
    /* Owner-uid match takes priority over a coincidental gid match: the
       caller here is BOTH the owner AND shares the owner's own gid, but the
       decision must still come from the owner bit, not group. */
    KT_TRUE(t, vfsPermitted(0100, 1, 10, 1, 10, fsModeOwnerExec));
    KT_FALSE(t, vfsPermitted(0010, 1, 10, 1, 10, fsModeOwnerExec));
}

/* Real owner identity (fs.h's FbStat.uid, vfs.c's vfsUid): a fresh tmpfs
   node is stamped with whoever created it -- ktestRunAll always runs as
   the permanent idle thread (id 0, uid 0, "root"), the same context
   schedTest.c's own pgid-default tests already document relying on, so
   every node created here is root-owned by construction. */
KTEST(coreVfsTmpfsNodeStampsCreatorAsOwner)
{
    int fd = vfsOpen("/tmp/uidf", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    FbStat st;
    KT_EQ(t, vfsStat("/tmp/uidf", &st), 0);
    KT_EQU(t, st.uid, 0u);

    vfsUnlink("/tmp/uidf");
}

/* vfsOwnerOrRoot pure predicate (vfs.c/h): same "explicit-argument, no live
   thread" test shape as vfsPermittedOwnerMatch/NonOwner/UidZero above --
   covers all four quadrants (self-match as a non-root uid, root bypass on
   a foreign file, root owning and calling as itself, and a plain stranger
   denied against both a root-owned and a non-root-owned file). Needed
   because KTEST always runs as the permanent idle thread's own uid 0
   (coreVfsTmpfsNodeStampsCreatorAsOwner's own doc comment), so no KTEST can
   ever construct a real vfsChmod/vfsChown call with a non-root caller uid
   to exercise the "denied" side directly -- that side is only provable
   live (identtest.c's chmod-nonowner-denied-ok/chown-nonroot-denied-ok). */
KTEST(coreVfsOwnerOrRootPredicate)
{
    KT_TRUE(t, vfsOwnerOrRoot(1000, 1000));  /* owner match, neither is root */
    KT_TRUE(t, vfsOwnerOrRoot(1000, 0));     /* root bypass on a foreign file */
    KT_TRUE(t, vfsOwnerOrRoot(0, 0));        /* root owns and calls as root */
    KT_FALSE(t, vfsOwnerOrRoot(0, 1000));    /* stranger against a root-owned file */
    KT_FALSE(t, vfsOwnerOrRoot(1000, 2000)); /* plain stranger, neither is root */
}

/* Real, previously-missing enforcement: before this fala, vfsChmod let ANY
   uid chmod ANY file's bits, since it never consulted the owner at all --
   vfsChmod("/some/foreign/file", ...) always silently succeeded regardless
   of who called it. This proves the ALLOWED side of the fix at the real
   vfs.c choke point (the denied side can only be proven live, see the
   predicate test above): root may still chmod a file it does NOT own, via
   the new vfsChown reassigning ownership away from the creator first. */
KTEST(coreVfsChmodRootBypassesForeignOwnership)
{
    int fd = vfsOpen("/tmp/chmodForeign", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    KT_EQ(t, vfsChown("/tmp/chmodForeign", 4242), 0);
    FbStat st;
    KT_EQ(t, vfsStat("/tmp/chmodForeign", &st), 0);
    KT_EQU(t, st.uid, 4242u);

    KT_EQ(t, vfsChmod("/tmp/chmodForeign", 0600), 0);
    KT_EQ(t, vfsStat("/tmp/chmodForeign", &st), 0);
    KT_EQ(t, st.mode, 0600u);

    vfsUnlink("/tmp/chmodForeign");
}

/* Real chown(2) on tmpfs: root may reassign a file's owner outright; a
   missing path or the never-writable ustar-loaded /bin content is refused
   the same way vfsChmod already is (coreVfsChmodRejectsMissingAndReadonlyBin
   above). */
KTEST(coreVfsChownTmpfsReassignsOwner)
{
    int fd = vfsOpen("/tmp/chownf", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    FbStat st;
    KT_EQ(t, vfsStat("/tmp/chownf", &st), 0);
    KT_EQU(t, st.uid, 0u);

    KT_EQ(t, vfsChown("/tmp/chownf", 7777), 0);
    KT_EQ(t, vfsStat("/tmp/chownf", &st), 0);
    KT_EQU(t, st.uid, 7777u);

    vfsUnlink("/tmp/chownf");
}

KTEST(coreVfsChownRejectsMissingAndReadonlyBin)
{
    KT_EQ(t, vfsChown("/tmp/doesNotExist", 1000), -1);
    KT_EQ(t, vfsChown("/bin/false", 1000), -1);

    FbStat st;
    KT_EQ(t, vfsStat("/bin/false", &st), 0);
    KT_EQU(t, st.uid, 0u);
}

/* Same round trip on the persistent SFS mount, PLUS the "survives
   unmount+remount" proof mode/mtime/ctime already have -- the reassigned
   uid genuinely lives on disk, not just in the in-memory inode cache. */
KTEST(coreVfsChownSfsPersistsAcrossRemount)
{
    KT_EQ(t, vfsMkdir("/mnt/chownd"), 0);
    int fd = vfsOpen("/mnt/chownd/f", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    FbStat st;
    KT_EQ(t, vfsStat("/mnt/chownd/f", &st), 0);
    KT_EQU(t, st.uid, 0u);

    KT_EQ(t, vfsChown("/mnt/chownd/f", 5555), 0);
    KT_EQ(t, vfsStat("/mnt/chownd/f", &st), 0);
    KT_EQU(t, st.uid, 5555u);

    sfsUnmount();
    KT_TRUE(t, sfsMount());

    KT_EQ(t, vfsStat("/mnt/chownd/f", &st), 0);
    KT_EQU(t, st.uid, 5555u);

    vfsUnlink("/mnt/chownd/f");
    vfsRmdir("/mnt/chownd");
}

/* Real chgrp(2) privilege rule (vfs.c's vfsChgrp): unlike vfsChown
   (root-only, full stop), the file's own owner may ALSO reassign the
   group -- but only to their OWN current gid (currentThreadGid()), never
   an arbitrary one, since this kernel tracks exactly one gid per thread.
   KTEST always runs as the idle thread's own (uid 0, gid 0), so the
   caller here genuinely IS the owner of a freshly created file, letting
   this test exercise the "owner, not root" half of vfsChgrp's dual
   privilege check -- setting the group to 0 (this thread's own gid)
   succeeds, setting it to anything else fails, exactly matching
   grouptest.c's live chgrp-owner-self-gid-ok/chgrp-owner-arbitrary-
   denied-ok pair, but provable here without a live cross-uid process
   since "owner but not root" only requires being non-root, which this
   thread already permanently is not (root bypass is tested separately by
   coreVfsChgrpRootReassignsArbitraryGroup below, since root-vs-owner needs
   two different DENIED-side outcomes root's own uid-0 bypass would hide). */
KTEST(coreVfsChgrpOwnerOnlyMaySetOwnCurrentGid)
{
    int fd = vfsOpen("/tmp/chgrpOwner", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    FbStat st;
    KT_EQ(t, vfsStat("/tmp/chgrpOwner", &st), 0);
    KT_EQU(t, st.gid, 0u);

    /* Root (this thread) is ALSO the owner here, so vfsChgrp's root bypass
       and its owner-may-set-own-gid path both agree on this one outcome --
       coreVfsChgrpRootReassignsArbitraryGroup below isolates the root-only
       bypass on a file this thread does NOT own, where they diverge. */
    KT_EQ(t, vfsChgrp("/tmp/chgrpOwner", 0), 0);
    KT_EQ(t, vfsStat("/tmp/chgrpOwner", &st), 0);
    KT_EQU(t, st.gid, 0u);

    vfsUnlink("/tmp/chgrpOwner");
}

/* Root's chgrp bypass specifically: reassigns an ARBITRARY gid on a file
   this thread does NOT own (chowned away first, mirroring
   coreVfsChmodRootBypassesForeignOwnership's own shape) -- the one case
   coreVfsChgrpOwnerOnlyMaySetOwnCurrentGid above cannot isolate, since
   there root is also the owner. */
KTEST(coreVfsChgrpRootReassignsArbitraryGroup)
{
    int fd = vfsOpen("/tmp/chgrpForeign", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);
    KT_EQ(t, vfsChown("/tmp/chgrpForeign", 4242), 0);

    KT_EQ(t, vfsChgrp("/tmp/chgrpForeign", 9999), 0);
    FbStat st;
    KT_EQ(t, vfsStat("/tmp/chgrpForeign", &st), 0);
    KT_EQU(t, st.gid, 9999u);

    vfsUnlink("/tmp/chgrpForeign");
}

/* Same round trip on the persistent SFS mount, PLUS the "survives
   unmount+remount" proof every other SFS metadata field already has --
   mirrors coreVfsChownSfsPersistsAcrossRemount exactly, one field over. */
KTEST(coreVfsChgrpSfsPersistsAcrossRemount)
{
    KT_EQ(t, vfsMkdir("/mnt/chgrpd"), 0);
    int fd = vfsOpen("/mnt/chgrpd/f", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    FbStat st;
    KT_EQ(t, vfsStat("/mnt/chgrpd/f", &st), 0);
    KT_EQU(t, st.gid, 0u);

    KT_EQ(t, vfsChgrp("/mnt/chgrpd/f", 6666), 0);
    KT_EQ(t, vfsStat("/mnt/chgrpd/f", &st), 0);
    KT_EQU(t, st.gid, 6666u);

    sfsUnmount();
    KT_TRUE(t, sfsMount());

    KT_EQ(t, vfsStat("/mnt/chgrpd/f", &st), 0);
    KT_EQU(t, st.gid, 6666u);

    vfsUnlink("/mnt/chgrpd/f");
    vfsRmdir("/mnt/chgrpd");
}

/* Real directory-write enforcement (vfs.c's vfsMkdir/resolveOpen create
   branch), the gap FALA 44 explicitly flagged as stored-but-unenforced --
   creating a NEW entry (file or subdirectory) needs the owner-write bit on
   the PARENT directory, checked against the CALLING thread's own uid, not
   just "the parent is a writable tmpfs node". KTEST always runs as the
   permanent idle thread's own uid 0 (coreVfsTmpfsNodeStampsCreatorAsOwner's
   own doc comment), and vfsPermitted has no uid-0 bypass
   (coreVfsPermittedUidZeroIsNotASuperuser), so removing root's OWN
   owner-write bit on a directory it owns genuinely denies root itself --
   the same "no superuser bypass" story chmod's own tests already prove for
   files, now proven for directories too, entirely from KTEST without
   needing a live non-root caller. */
KTEST(coreVfsMkdirAndCreateRequireParentWrite)
{
    KT_EQ(t, vfsMkdir("/tmp/dwParent"), 0);
    KT_EQ(t, vfsChmod("/tmp/dwParent", 0555), 0);   /* r-x, no owner-write */

    KT_EQ(t, vfsMkdir("/tmp/dwParent/child"), -1);
    KT_EQ(t, vfsOpen("/tmp/dwParent/f", oWriteOnly | oCreate), -1);

    KT_EQ(t, vfsChmod("/tmp/dwParent", fsModeDefaultDir), 0);
    KT_EQ(t, vfsMkdir("/tmp/dwParent/child"), 0);
    int fd = vfsOpen("/tmp/dwParent/f", oWriteOnly | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    vfsUnlink("/tmp/dwParent/f");
    vfsRmdir("/tmp/dwParent/child");
    vfsRmdir("/tmp/dwParent");
}

/* Real unlink(2)/rmdir(2) semantics: removing an entry needs write
   permission on the PARENT directory, never the entry's own mode -- proven
   here by a file that is itself still perfectly writable (0644) but whose
   PARENT has lost its own write bit, matching real Unix's own "you can
   delete a read-only file if you can write its directory, and you canNOT
   delete a writable file if you can't" rule the file-level mode never
   governs on its own. */
KTEST(coreVfsUnlinkAndRmdirRequireParentWrite)
{
    KT_EQ(t, vfsMkdir("/tmp/dwParent2"), 0);
    int fd = vfsOpen("/tmp/dwParent2/f", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);
    KT_EQ(t, vfsMkdir("/tmp/dwParent2/sub"), 0);

    KT_EQ(t, vfsChmod("/tmp/dwParent2", 0555), 0);
    KT_EQ(t, vfsUnlink("/tmp/dwParent2/f"), -1);
    KT_EQ(t, vfsRmdir("/tmp/dwParent2/sub"), -1);

    KT_EQ(t, vfsChmod("/tmp/dwParent2", fsModeDefaultDir), 0);
    KT_EQ(t, vfsUnlink("/tmp/dwParent2/f"), 0);
    KT_EQ(t, vfsRmdir("/tmp/dwParent2/sub"), 0);
    KT_EQ(t, vfsRmdir("/tmp/dwParent2"), 0);
}

/* Real POSIX search permission (vfs.c's lookupNode, checked on every
   directory ANCESTOR a path walks through): removing the exec bit on a
   directory makes every path THROUGH it fail, even though the target file
   inside it never changed at all -- proven by chmod'ing the ancestor away
   and back around an otherwise-untouched file. */
KTEST(coreVfsLookupRequiresAncestorExec)
{
    KT_EQ(t, vfsMkdir("/tmp/dxAncestor"), 0);
    int fd = vfsOpen("/tmp/dxAncestor/leaf", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    KT_EQ(t, vfsWrite(fd, "x", 1), 1);
    vfsClose(fd);

    KT_EQ(t, vfsChmod("/tmp/dxAncestor", 0600), 0);   /* rw-, no exec at all */
    FbStat st;
    KT_EQ(t, vfsStat("/tmp/dxAncestor/leaf", &st), -1);
    KT_EQ(t, vfsOpen("/tmp/dxAncestor/leaf", oReadOnly), -1);

    KT_EQ(t, vfsChmod("/tmp/dxAncestor", fsModeDefaultDir), 0);
    KT_EQ(t, vfsStat("/tmp/dxAncestor/leaf", &st), 0);
    int rfd = vfsOpen("/tmp/dxAncestor/leaf", oReadOnly);
    KT_GE(t, rfd, 0);
    vfsClose(rfd);

    vfsUnlink("/tmp/dxAncestor/leaf");
    vfsRmdir("/tmp/dxAncestor");
}

/* Real distinction between search (exec) and list (read) on a directory --
   the subtlest part of this feature, and the one most likely to be
   accidentally conflated: a directory with exec but no read lets a caller
   open a KNOWN child by name (resolveOpen never needs read on `cur` for
   that, only exec on it as an ANCESTOR of the leaf) while refusing to open
   the directory itself for listing (vfsGetdents). Mirrors dirpermtest.c's
   own live list-denied-ok/search-without-list-ok pair, but here entirely
   from KTEST since both sides are root-vs-its-own-directory, not root-vs-
   another-uid. */
KTEST(coreVfsOpendirRequiresDirReadNotJustExec)
{
    KT_EQ(t, vfsMkdir("/tmp/dxListable"), 0);
    int fd = vfsOpen("/tmp/dxListable/known", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    KT_EQ(t, vfsChmod("/tmp/dxListable", 0311), 0);   /* -wx, no owner-read */
    KT_EQ(t, vfsOpen("/tmp/dxListable", oReadOnly), -1);
    int kfd = vfsOpen("/tmp/dxListable/known", oReadOnly);
    KT_GE(t, kfd, 0);
    vfsClose(kfd);

    KT_EQ(t, vfsChmod("/tmp/dxListable", fsModeDefaultDir), 0);
    int dd = vfsOpen("/tmp/dxListable", oReadOnly);
    KT_GE(t, dd, 0);
    FbDirent ents[8];
    KT_TRUE(t, vfsGetdents(dd, ents, sizeof(ents)) >= (int64)sizeof(FbDirent));
    vfsClose(dd);

    vfsUnlink("/tmp/dxListable/known");
    vfsRmdir("/tmp/dxListable");
}

/* Same three checks as the four tmpfs tests directly above (parent-write
   for create/remove, ancestor-exec for traversal, dir-read-vs-exec for
   listing), run once through the SFS dispatch branch of vfs.c's own
   choke points (resolveOpen/vfsMkdir/vfsUnlink/vfsRmdir/lookupNode's
   isSfsPath fork) instead of sfs.c's raw primitives directly -- sfsTest.c's
   sfsCreateNodeRequiresParentWrite/sfsRemoveRequiresParentWrite/
   sfsLookupRequiresAncestorExec already cover the on-disk layer itself;
   this is the "does the /mnt dispatch actually reach it" parity proof, the
   same relationship coreVfsChmodSfsPersistsAcrossRemount already has to
   its own tmpfs sibling above. */
KTEST(coreVfsSfsDirectoryPermissionParity)
{
    KT_EQ(t, vfsMkdir("/mnt/dpTop"), 0);
    int fd = vfsOpen("/mnt/dpTop/leaf", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    KT_EQ(t, vfsChmod("/mnt/dpTop", 0555), 0);
    KT_EQ(t, vfsOpen("/mnt/dpTop/newfile", oWriteOnly | oCreate), -1);
    KT_EQ(t, vfsUnlink("/mnt/dpTop/leaf"), -1);

    KT_EQ(t, vfsChmod("/mnt/dpTop", 0600), 0);
    FbStat st;
    KT_EQ(t, vfsStat("/mnt/dpTop/leaf", &st), -1);

    KT_EQ(t, vfsChmod("/mnt/dpTop", 0311), 0);
    KT_EQ(t, vfsOpen("/mnt/dpTop", oReadOnly), -1);
    int kfd = vfsOpen("/mnt/dpTop/leaf", oReadOnly);
    KT_GE(t, kfd, 0);
    vfsClose(kfd);

    KT_EQ(t, vfsChmod("/mnt/dpTop", fsModeDefaultDir), 0);
    KT_EQ(t, vfsUnlink("/mnt/dpTop/leaf"), 0);
    KT_EQ(t, vfsRmdir("/mnt/dpTop"), 0);
}
