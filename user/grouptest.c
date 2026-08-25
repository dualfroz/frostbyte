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

    /* --- real setgid(2): root may set its own gid to anything. --- */
    if (userSetgid(7000) != 0) {
        print("grouptest: FAIL root could not setgid(7000)\n");
        return 1;
    }
    if (userGetgid() != 7000) {
        print("grouptest: FAIL gid did not actually change to 7000\n");
        return 1;
    }
    print("grouptest: setgid-root-arbitrary-ok\n");

    /* --- real, permanent privilege drop: root becoming a plain uid, gid
       already set above -- both credentials independent, matching real
       Unix's separate uid/gid drop order. --- */
    if (userSetuid(6000) != 0) {
        print("grouptest: FAIL root could not setuid(6000)\n");
        return 1;
    }
    if (userGetuid() != 6000 || userGetgid() != 7000) {
        print("grouptest: FAIL identity after drop is not (6000,7000)\n");
        return 1;
    }
    print("grouptest: drop-to-6000-7000-ok\n");

    /* --- no way back: setgid is a one-way trapdoor too, same shape as
       setuid's own cannot-regain-root-ok. --- */
    if (userSetgid(9999) != -1) {
        print("grouptest: FAIL non-root regained arbitrary gid via setgid\n");
        return 1;
    }
    if (userGetgid() != 7000) {
        print("grouptest: FAIL gid changed despite rejected setgid\n");
        return 1;
    }
    print("grouptest: setgid-nonroot-denied-ok\n");

    if (userSetgid(7000) != 0) {
        print("grouptest: FAIL setgid to own current gid was rejected\n");
        return 1;
    }
    print("grouptest: setgid-self-noop-ok\n");

    /* --- the actual point of this fala: a caller that is NOT the owner
       (uid 6000 vs 9000) but DOES share the file's group (7000) is
       governed by the group bits, not other. groupOnly has ONLY
       group-read set, so read succeeds and write fails. --- */
    int rfd = (int)userOpen("/tmp/groupd/groupOnly", oReadOnly);
    if (rfd < 0) {
        print("grouptest: FAIL group-matching read denied\n");
        return 1;
    }
    userClose(rfd);
    print("grouptest: group-read-allowed-ok\n");

    if (userOpen("/tmp/groupd/groupOnly", oWriteOnly) >= 0) {
        print("grouptest: FAIL group-write succeeded without the bit\n");
        return 1;
    }
    print("grouptest: group-write-denied-ok\n");

    /* --- a caller matching NEITHER uid (6000 vs 9000) NOR gid (7000 vs
       8000) must fall through to other, which mismatch also leaves
       clear. --- */
    if (userOpen("/tmp/groupd/mismatch", oReadOnly) >= 0) {
        print("grouptest: FAIL non-member read succeeded via wrong branch\n");
        return 1;
    }
    print("grouptest: other-fallback-denied-ok\n");

    /* --- the same group model gates real exec, not just open(). --- */
    long pid = userSpawn("/tmp/groupd/progOk", 0);
    if (pid < 0) {
        print("grouptest: FAIL group-exec spawn denied\n");
        return 1;
    }
    if (userWait(pid) != 1) {
        print("grouptest: FAIL group-exec spawned copy did not exit 1\n");
        return 1;
    }
    print("grouptest: group-exec-allowed-ok\n");

    if (userSpawn("/tmp/groupd/progDenied", 0) >= 0) {
        print("grouptest: FAIL group-exec spawn succeeded across groups\n");
        return 1;
    }
    print("grouptest: group-exec-denied-ok\n");

    /* --- real chgrp(2) privilege: the owner (this process really does own
       "owned" now, uid 6000) may set the group to its OWN current gid
       (7000) -- but not to anything else, even though it owns the
       file. --- */
    if (userChgrp("/tmp/groupd/owned", 7000) != 0) {
        print("grouptest: FAIL owner chgrp to own gid denied\n");
        return 1;
    }
    if (userStat("/tmp/groupd/owned", &st) != 0 || st.gid != 7000) {
        print("grouptest: FAIL owned gid not actually reassigned\n");
        return 1;
    }
    print("grouptest: chgrp-owner-self-gid-ok\n");

    if (userChgrp("/tmp/groupd/owned", 8000) != -1) {
        print("grouptest: FAIL owner chgrp to arbitrary gid succeeded\n");
        return 1;
    }
    if (userStat("/tmp/groupd/owned", &st) != 0 || st.gid != 7000) {
        print("grouptest: FAIL owned gid changed by denied chgrp\n");
        return 1;
    }
    print("grouptest: chgrp-owner-arbitrary-denied-ok\n");

    /* --- a non-owner, non-root caller can never chgrp at all, even to its
       own gid -- mismatch is owned by 9000, this process is neither owner
       nor root. --- */
    if (userChgrp("/tmp/groupd/mismatch", 7000) != -1) {
        print("grouptest: FAIL non-owner non-root chgrp succeeded\n");
        return 1;
    }
    if (userStat("/tmp/groupd/mismatch", &st) != 0 || st.gid != 8000) {
        print("grouptest: FAIL mismatch gid changed by denied chgrp\n");
        return 1;
    }
    print("grouptest: chgrp-nonowner-nonroot-denied-ok\n");

    print("grouptest: PASS\n");
    return 0;
}
