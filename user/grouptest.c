#include "ulib.h"
#include "fs.h"

/*
 * Real ring-3 proof of the group credential (sched.h's Thread.gid,
 * currentThreadGid/currentThreadSetGid) and the new middle branch of
 * vfs.c's vfsPermitted (owner-vs-group-vs-other) -- coreTest.c's
 * coreVfsPermittedGroupMatchChecksGroupBitOnly can only exercise the pure
 * predicate directly, since KTEST always runs as the permanent idle
 * thread's own (uid 0, gid 0). This is the live proof that a caller who is
 * NOT a file's owner but DOES share its group gid gets governed by the
 * fsModeGroup* bits specifically -- neither the owner bits (wrong uid) nor
 * the other bits (deliberately left clear below) -- and that a caller
 * matching NEITHER uid nor gid correctly falls through to other. Also
 * proves real chgrp(2) (userChgrp, vfs.c's vfsChgrp): root may reassign
 * any gid, but a non-root owner may only ever set the group to its OWN
 * current gid. Fully synchronous like identtest.c/dirpermtest.c: no
 * injected keystrokes, no shared shell channel, so there is no
 * scheduling-timing race to flake on regardless of host load.
 */

static int copyFile(const char* srcPath, const char* dstPath)
{
    int src = (int)userOpen(srcPath, oReadOnly);
    if (src < 0)
        return -1;
    int dst = (int)userOpen(dstPath, oReadWrite | oCreate | oTrunc);
    if (dst < 0) {
        userClose(src);
        return -1;
    }

    char buf[512];
    long got;
    while ((got = userRead(src, buf, sizeof(buf))) > 0) {
        if (userWrite(dst, buf, (unsigned long)got) != got) {
            userClose(src);
            userClose(dst);
            return -1;
        }
    }
    userClose(src);
    userClose(dst);
    return got < 0 ? -1 : 0;
}

int main(void)
{
    print("grouptest: start, pid=");
    printDec(userGetpid());
    print("\n");

    /* --- boot-time default: every process starts at gid 0, alongside the
       already-proven uid 0 (identtest.c). --- */
    if (userGetuid() != 0 || userGetgid() != 0) {
        print("grouptest: FAIL boot-time uid/gid is not 0\n");
        return 1;
    }
    print("grouptest: root-by-default-ok\n");

    if (userMkdir("/tmp/groupd") != 0) {
        print("grouptest: FAIL mkdir /tmp/groupd\n");
        return 1;
    }

    /* --- a fresh file is stamped with the CREATOR's current gid, exactly
       like uid already was. --- */
    int fd = (int)userOpen("/tmp/groupd/stamp", oReadWrite | oCreate);
    if (fd < 0) {
        print("grouptest: FAIL create stamp\n");
        return 1;
    }
    userClose(fd);
    FbStat st;
    if (userStat("/tmp/groupd/stamp", &st) != 0 || st.gid != 0) {
        print("grouptest: FAIL fresh file not stamped with creator gid\n");
        return 1;
    }
    print("grouptest: fresh-file-gid-stamped-ok\n");

    /* --- set up every file this process will need access to AFTER its own
       permanent identity drop, exactly like identtest.c's owned.txt has to
       be chown'd before dropping: once this process is no longer root,
       neither chown nor an arbitrary chgrp will ever work again. ---

       groupOnly: owned by a stranger (9000), group 7000 (the gid THIS
       process will adopt), mode 0040 (group-read only, nothing else). */
    fd = (int)userOpen("/tmp/groupd/groupOnly", oReadWrite | oCreate);
    if (fd < 0) {
        print("grouptest: FAIL create groupOnly\n");
        return 1;
    }
    userClose(fd);
    if (userChown("/tmp/groupd/groupOnly", 9000) != 0 ||
        userChgrp("/tmp/groupd/groupOnly", 7000) != 0 ||
        userChmod("/tmp/groupd/groupOnly", 0040) != 0) {
        print("grouptest: FAIL set up groupOnly\n");
        return 1;
    }

    /* mismatch: owned by a stranger (9000), group 8000 (NOT the gid this
       process will adopt), mode 0040 (group-read only) -- so after the
       drop, neither uid nor gid matches, and other has nothing either. */
    fd = (int)userOpen("/tmp/groupd/mismatch", oReadWrite | oCreate);
    if (fd < 0) {
        print("grouptest: FAIL create mismatch\n");
        return 1;
    }
    userClose(fd);
    if (userChown("/tmp/groupd/mismatch", 9000) != 0 ||
        userChgrp("/tmp/groupd/mismatch", 8000) != 0 ||
        userChmod("/tmp/groupd/mismatch", 0040) != 0) {
        print("grouptest: FAIL set up mismatch\n");
        return 1;
    }
    print("grouptest: chgrp-root-arbitrary-ok\n");

    /* progOk: a copy of /bin/false, owned by 9000, group 7000, group-exec
       only -- proves the SAME group model gates exec (loadImageByPath/
       jobctlSpawn), not just open(). progDenied: identical except group
       8000, so it must refuse after the drop. */
    if (copyFile("/bin/false", "/tmp/groupd/progOk") != 0 ||
        copyFile("/bin/false", "/tmp/groupd/progDenied") != 0) {
        print("grouptest: FAIL copy /bin/false\n");
        return 1;
    }
    if (userChown("/tmp/groupd/progOk", 9000) != 0 ||
        userChgrp("/tmp/groupd/progOk", 7000) != 0 ||
        userChmod("/tmp/groupd/progOk", 0010) != 0) {
        print("grouptest: FAIL set up progOk\n");
        return 1;
    }
    if (userChown("/tmp/groupd/progDenied", 9000) != 0 ||
        userChgrp("/tmp/groupd/progDenied", 8000) != 0 ||
        userChmod("/tmp/groupd/progDenied", 0010) != 0) {
        print("grouptest: FAIL set up progDenied\n");
        return 1;
    }

    /* owned: THIS process's own future uid (6000) owns it outright,
       initial group 0 -- the target for the owner-chgrp tests below,
       mirroring identtest.c's owned.txt. */
    fd = (int)userOpen("/tmp/groupd/owned", oReadWrite | oCreate);
    if (fd < 0) {
        print("grouptest: FAIL create owned\n");
        return 1;
    }
    userClose(fd);
    if (userChown("/tmp/groupd/owned", 6000) != 0) {
        print("grouptest: FAIL chown owned to 6000\n");
        return 1;
    }

