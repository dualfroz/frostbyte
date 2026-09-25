#include "ktest.h"
#include "string.h"
#include "heap.h"
#include "pmm.h"
#include "vfs.h"
#include "fs.h"
#include "sfs.h"

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

    memmove(buf + 2, buf, 8);
    buf[10] = 0;
    KT_STREQ(t, buf, "0101234567");

    memcpy(buf, "0123456789", 11);
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

    kfree(b);
    void* d = kmalloc(150);
    KT_EQP(t, d, b);

    kfree(a);
    kfree(c);
    kfree(d);

    KT_GE(t, heapFreeBytes(), freeBefore);
}

KTEST(coreHeapRealloc)
{
    char* p = (char*)kmalloc(8);
    KT_NOTNULL(t, p);
    memcpy(p, "1234567", 8);

    p = (char*)krealloc(p, 64);
    KT_NOTNULL(t, p);
    KT_STREQ(t, p, "1234567");

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
    KT_EQ(t, f1 & (frameSize - 1), 0);

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
    KT_EQ(t, vfsStat("/tmp/coretest.txt", &st), -1);
}

KTEST(coreVfsMkdirGetdents)
{
    KT_EQ(t, vfsMkdir("/tmp/cd"), 0);
    KT_EQ(t, vfsMkdir("/tmp/cd"), -1);

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

KTEST(coreVfsRmdirTmpfsEmptyCheck)
{
    KT_EQ(t, vfsMkdir("/tmp/rd"), 0);

    int fd = vfsOpen("/tmp/rd/f", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    KT_EQ(t, vfsRmdir("/tmp/rd"), -1);
    FbStat st;
    KT_EQ(t, vfsStat("/tmp/rd", &st), 0);

    KT_EQ(t, vfsUnlink("/tmp/rd/f"), 0);
    KT_EQ(t, vfsRmdir("/tmp/rd"), 0);
    KT_EQ(t, vfsStat("/tmp/rd", &st), -1);
}

KTEST(coreVfsUnlinkAndRmdirRejectWrongTypeTmpfs)
{
    KT_EQ(t, vfsMkdir("/tmp/rdDir"), 0);
    KT_EQ(t, vfsUnlink("/tmp/rdDir"), -1);
    FbStat st;
    KT_EQ(t, vfsStat("/tmp/rdDir", &st), 0);
    KT_EQ(t, vfsRmdir("/tmp/rdDir"), 0);

    int fd = vfsOpen("/tmp/rdFile", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);
    KT_EQ(t, vfsRmdir("/tmp/rdFile"), -1);
    KT_EQ(t, vfsStat("/tmp/rdFile", &st), 0);
    KT_EQ(t, vfsUnlink("/tmp/rdFile"), 0);
}

KTEST(coreVfsRmdirRefusesRoot)
{
    KT_EQ(t, vfsRmdir("/"), -1);
}

KTEST(coreVfsRmdirSfsParity)
{
    KT_EQ(t, vfsMkdir("/mnt/rdSfsEmpty"), 0);
    KT_EQ(t, vfsUnlink("/mnt/rdSfsEmpty"), -1);
    FbStat st;
    KT_EQ(t, vfsStat("/mnt/rdSfsEmpty", &st), 0);
    KT_EQ(t, vfsRmdir("/mnt/rdSfsEmpty"), 0);
    KT_EQ(t, vfsStat("/mnt/rdSfsEmpty", &st), -1);

    KT_EQ(t, vfsMkdir("/mnt/rdSfsFull"), 0);
    int fd = vfsOpen("/mnt/rdSfsFull/f", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);
    KT_EQ(t, vfsRmdir("/mnt/rdSfsFull"), -1);
    KT_EQ(t, vfsUnlink("/mnt/rdSfsFull/f"), 0);
    KT_EQ(t, vfsRmdir("/mnt/rdSfsFull"), 0);

    fd = vfsOpen("/mnt/rdSfsFile", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);
    KT_EQ(t, vfsRmdir("/mnt/rdSfsFile"), -1);
    KT_EQ(t, vfsUnlink("/mnt/rdSfsFile"), 0);
}

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

    KT_EQ(t, vfsOpen("/tmp/chmodf", oWriteOnly), -1);
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

KTEST(coreVfsResolveOpenRequiresFileReadPermission)
{
    int fd = vfsOpen("/tmp/readf", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    KT_EQ(t, vfsWrite(fd, "x", 1), 1);
    vfsClose(fd);

    KT_EQ(t, vfsChmod("/tmp/readf", 0000), 0);
    KT_EQ(t, vfsOpen("/tmp/readf", oReadOnly), -1);
    KT_EQ(t, vfsOpen("/tmp/readf", oReadWrite), -1);

    KT_EQ(t, vfsChmod("/tmp/readf", 0200), 0);
    KT_EQ(t, vfsOpen("/tmp/readf", oReadOnly), -1);
    int wfd2 = vfsOpen("/tmp/readf", oWriteOnly);
    KT_GE(t, wfd2, 0);
    vfsClose(wfd2);

    KT_EQ(t, vfsChmod("/tmp/readf", fsModeDefaultFile), 0);
    int rfd2 = vfsOpen("/tmp/readf", oReadOnly);
    KT_GE(t, rfd2, 0);
    vfsClose(rfd2);

    vfsUnlink("/tmp/readf");
}

KTEST(coreVfsChmodRejectsMissingAndReadonlyBin)
{
    KT_EQ(t, vfsChmod("/tmp/doesNotExist", 0644), -1);
    KT_EQ(t, vfsChmod("/bin/false", 0777), -1);

    FbStat st;
    KT_EQ(t, vfsStat("/bin/false", &st), 0);
    KT_EQ(t, st.mode, fsModeDefaultExec);
}

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

KTEST(coreVfsPermittedOwnerMatchChecksOwnerBitOnly)
{
    KT_TRUE(t, vfsPermitted(0600, 1000, 0, 1000, 0, fsModeOwnerWrite));
    KT_FALSE(t, vfsPermitted(0004, 1000, 0, 1000, 0, fsModeOwnerWrite));
}

KTEST(coreVfsPermittedNonOwnerChecksOtherBitOnly)
{
    KT_TRUE(t, vfsPermitted(0002, 1000, 10, 2000, 20, fsModeOwnerWrite));
    KT_FALSE(t, vfsPermitted(0600, 1000, 10, 2000, 20, fsModeOwnerWrite));
    KT_FALSE(t, vfsPermitted(0000, 1000, 10, 2000, 20, fsModeOwnerExec));
}

KTEST(coreVfsPermittedUidZeroIsNotASuperuser)
{
    KT_TRUE(t, vfsPermitted(0600, 0, 0, 0, 0, fsModeOwnerWrite));
    KT_FALSE(t, vfsPermitted(0000, 0, 0, 0, 0, fsModeOwnerWrite));
    KT_FALSE(t, vfsPermitted(0700, 1000, 10, 0, 20, fsModeOwnerWrite));
    KT_TRUE(t, vfsPermitted(0702, 1000, 10, 0, 20, fsModeOwnerWrite));
    KT_FALSE(t, vfsPermitted(0600, 42, 10, 0, 20, fsModeOwnerWrite));
}

KTEST(coreVfsPermittedExecBitFollowsSameOwnerOtherSplit)
{
    KT_TRUE(t, vfsPermitted(fsModeOtherExec, 1, 10, 2, 20, fsModeOwnerExec));
    KT_FALSE(t, vfsPermitted(fsModeOwnerExec, 1, 10, 2, 20, fsModeOwnerExec));
    KT_TRUE(t, vfsPermitted(fsModeOwnerExec, 1, 10, 1, 10, fsModeOwnerExec));
}

KTEST(coreVfsPermittedGroupMatchChecksGroupBitOnly)
{
    KT_TRUE(t, vfsPermitted(0020, 1000, 10, 2000, 10, fsModeOwnerWrite));
    KT_FALSE(t, vfsPermitted(0602, 1000, 10, 2000, 10, fsModeOwnerWrite));
    KT_TRUE(t, vfsPermitted(fsModeGroupExec, 1, 10, 2, 10, fsModeOwnerExec));
    KT_TRUE(t, vfsPermitted(0100, 1, 10, 1, 10, fsModeOwnerExec));
    KT_FALSE(t, vfsPermitted(0010, 1, 10, 1, 10, fsModeOwnerExec));
}

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

KTEST(coreVfsOwnerOrRootPredicate)
{
    KT_TRUE(t, vfsOwnerOrRoot(1000, 1000));
    KT_TRUE(t, vfsOwnerOrRoot(1000, 0));
    KT_TRUE(t, vfsOwnerOrRoot(0, 0));
    KT_FALSE(t, vfsOwnerOrRoot(0, 1000));
    KT_FALSE(t, vfsOwnerOrRoot(1000, 2000));
}

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

KTEST(coreVfsChgrpOwnerOnlyMaySetOwnCurrentGid)
{
    int fd = vfsOpen("/tmp/chgrpOwner", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    FbStat st;
    KT_EQ(t, vfsStat("/tmp/chgrpOwner", &st), 0);
    KT_EQU(t, st.gid, 0u);

    KT_EQ(t, vfsChgrp("/tmp/chgrpOwner", 0), 0);
    KT_EQ(t, vfsStat("/tmp/chgrpOwner", &st), 0);
    KT_EQU(t, st.gid, 0u);

    vfsUnlink("/tmp/chgrpOwner");
}

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

KTEST(coreVfsMkdirAndCreateRequireParentWrite)
{
    KT_EQ(t, vfsMkdir("/tmp/dwParent"), 0);
    KT_EQ(t, vfsChmod("/tmp/dwParent", 0555), 0);

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

KTEST(coreVfsLookupRequiresAncestorExec)
{
    KT_EQ(t, vfsMkdir("/tmp/dxAncestor"), 0);
    int fd = vfsOpen("/tmp/dxAncestor/leaf", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    KT_EQ(t, vfsWrite(fd, "x", 1), 1);
    vfsClose(fd);

    KT_EQ(t, vfsChmod("/tmp/dxAncestor", 0600), 0);
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

KTEST(coreVfsOpendirRequiresDirReadNotJustExec)
{
    KT_EQ(t, vfsMkdir("/tmp/dxListable"), 0);
    int fd = vfsOpen("/tmp/dxListable/known", oReadWrite | oCreate);
    KT_GE(t, fd, 0);
    vfsClose(fd);

    KT_EQ(t, vfsChmod("/tmp/dxListable", 0311), 0);
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
