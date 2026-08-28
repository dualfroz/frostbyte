#include "ulib.h"
#include "fs.h"

/*
 * Real ring-3 proof of the uid credential (sched.h's Thread.uid,
 * currentThreadUid/currentThreadSetUid) and the owner/other half of
 * vfs.c's vfsPermitted: userGetuid/userSetuid (real getuid(2)/setuid(2)),
 * a genuine, permanent, one-way privilege drop from root (uid 0) to a
 * plain uid, and the resulting real EACCES-shaped refusals once this same
 * process is no longer the owner of files it itself created moments
 * earlier as root. Also proves real chown(2) (userChown, vfs.c's
 * vfsChown, root-only per _POSIX_CHOWN_RESTRICTED) and the fix to a real,
 * previously-missing bug: vfsChmod used to let ANY uid chmod ANY file's
 * bits, since it never consulted ownership at all -- this is the live,
 * ring-3 proof that a non-owner, non-root chmod is now genuinely refused,
 * not just unit-tested against a hand-picked uid pair (coreTest.c's
 * coreVfsOwnerOrRootPredicate can only exercise the pure predicate, since
 * KTEST always runs as the permanent idle thread's own uid 0). Fully
 * synchronous like permtest.c/rmtest.c/timetest.c: no injected keystrokes,
 * no shared shell channel, so there is no scheduling-timing race to flake
 * on regardless of host load.
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
    print("identtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    /* --- boot-time default: every process starts as uid 0, and a freshly
       created file is owned by whoever created it. --- */
    if (userGetuid() != 0) {
        print("identtest: FAIL boot-time uid is not 0\n");
        return 1;
    }
    print("identtest: root-by-default-ok\n");

    if (userMkdir("/tmp/identd") != 0) {
        print("identtest: FAIL mkdir /tmp/identd\n");
        return 1;
    }
    int fd = (int)userOpen("/tmp/identd/secret.txt", oReadWrite | oCreate);
    if (fd < 0) {
        print("identtest: FAIL create secret.txt\n");
        return 1;
    }
    if (userWrite(fd, "root-only", 9) != 9) {
        print("identtest: FAIL initial write\n");
        return 1;
    }
    userClose(fd);

    FbStat st;
    if (userStat("/tmp/identd/secret.txt", &st) != 0 || st.uid != 0) {
        print("identtest: FAIL fresh file not owned by uid 0\n");
        return 1;
    }
    print("identtest: owner-root-ok\n");

    /* Owner-only rw, nothing for anyone else -- while still root, this
       process is the OWNER, so the owner bit (still set) governs and
       write access works normally through vfsPermitted's owner-match
       branch, not through any special uid-0 treatment (there is none). */
    if (userChmod("/tmp/identd/secret.txt", 0600) != 0) {
        print("identtest: FAIL chmod to owner-only\n");
        return 1;
    }
    int wfd = (int)userOpen("/tmp/identd/secret.txt", oWriteOnly);
    if (wfd < 0) {
        print("identtest: FAIL owner write denied while still owner\n");
        return 1;
    }
    userClose(wfd);
    print("identtest: owner-write-ok-before-drop\n");

    /* --- a copy of /bin/false, owned by root, chmod'd to owner-exec only
       (no other-exec) -- spawnable now, while this process is still
       uid 0 and therefore still the owner. --- */
    if (copyFile("/bin/false", "/tmp/identd/prog") != 0) {
        print("identtest: FAIL copy /bin/false to prog\n");
        return 1;
    }
    if (userChmod("/tmp/identd/prog", 0100) != 0) {
        print("identtest: FAIL chmod prog to owner-exec-only\n");
        return 1;
    }
    long pid = userSpawn("/tmp/identd/prog", 0);
    if (pid < 0) {
        print("identtest: FAIL spawn denied while still owner\n");
        return 1;
    }
    if (userWait(pid) != 1) {
        print("identtest: FAIL spawned copy did not exit 1\n");
        return 1;
    }
    print("identtest: owner-exec-ok-before-drop\n");

    /* --- real chown(2), root-only: a second file, still owned by root at
       creation, handed to uid 1000 BEFORE this process ever drops its own
       privilege. Kept separate from secret.txt (which stays root-owned
       throughout) so the post-drop non-owner checks below have both a
       file this process will NEVER own (secret.txt) and one it WILL
       (owned.txt), proving the two different denial shapes distinctly. --- */
    int ofd = (int)userOpen("/tmp/identd/owned.txt", oReadWrite | oCreate);
    if (ofd < 0) {
        print("identtest: FAIL create owned.txt\n");
        return 1;
    }
    userClose(ofd);
    if (userChown("/tmp/identd/owned.txt", 1000) != 0) {
        print("identtest: FAIL root chown owned.txt to 1000\n");
        return 1;
    }
    if (userStat("/tmp/identd/owned.txt", &st) != 0 || st.uid != 1000) {
        print("identtest: FAIL owned.txt uid not reassigned\n");
        return 1;
    }
    print("identtest: chown-root-ok\n");

    /* --- real, permanent privilege drop: root becoming a plain uid. --- */
    if (userSetuid(1000) != 0) {
        print("identtest: FAIL root could not setuid(1000)\n");
        return 1;
    }
    if (userGetuid() != 1000) {
        print("identtest: FAIL uid did not actually change to 1000\n");
        return 1;
    }
    print("identtest: drop-to-1000-ok\n");

    /* --- there is no way back: this is a one-way trapdoor, not a toggle. --- */
    if (userSetuid(0) != -1) {
        print("identtest: FAIL non-root regained root via setuid\n");
        return 1;
    }
    if (userGetuid() != 1000) {
        print("identtest: FAIL uid changed despite rejected setuid\n");
        return 1;
    }
    print("identtest: cannot-regain-root-ok\n");

    /* --- setting your own current uid is always a no-op success, exactly
       like real setuid(getuid()). --- */
    if (userSetuid(1000) != 0) {
        print("identtest: FAIL setuid to own current uid was rejected\n");
        return 1;
    }
    print("identtest: setuid-self-noop-ok\n");

    /* --- now a non-owner (uid 1000 against a uid-0-owned file): the
       owner-only 0600 mode has no other-write bit, so this write must be
       refused, even though this exact process wrote the same file
       successfully moments ago as its owner. --- */
    if (userOpen("/tmp/identd/secret.txt", oWriteOnly) >= 0) {
        print("identtest: FAIL write succeeded after dropping ownership\n");
        return 1;
    }
    print("identtest: write-denied-non-owner-ok\n");

    /* --- same story for exec: owner-exec-only, no other-exec, so the
       exact same binary this process spawned successfully as root now
       refuses to spawn at all. --- */
    if (userSpawn("/tmp/identd/prog", 0) >= 0) {
        print("identtest: FAIL spawn succeeded after dropping ownership\n");
        return 1;
    }
    print("identtest: exec-denied-non-owner-ok\n");

    /* --- real, previously-missing enforcement: before this fala, chmod had
       no ownership check at all, so ANY uid could chmod ANY file. Now that
       vfsChmod refuses a non-owner, non-root caller, this must fail against
       secret.txt (owned by root, this process is neither the owner nor
       root anymore) -- and the mode must be left completely unchanged. --- */
    if (userChmod("/tmp/identd/secret.txt", 0666) != -1) {
        print("identtest: FAIL non-owner chmod succeeded on secret.txt\n");
        return 1;
    }
    if (userStat("/tmp/identd/secret.txt", &st) != 0 || st.mode != 0600) {
        print("identtest: FAIL secret.txt mode changed by denied chmod\n");
        return 1;
    }
    print("identtest: chmod-nonowner-denied-ok\n");

    /* --- chown is root-only, full stop -- not even trying to chown a file
       this process does not own is any different from trying on one it
       does (see chown-owner-nonroot-denied-ok below): both are refused for
       the exact same reason, non-root. secret.txt's uid must stay 0. --- */
    if (userChown("/tmp/identd/secret.txt", 1000) != -1) {
        print("identtest: FAIL non-root chown succeeded on secret.txt\n");
        return 1;
    }
    if (userStat("/tmp/identd/secret.txt", &st) != 0 || st.uid != 0) {
        print("identtest: FAIL secret.txt uid changed by denied chown\n");
        return 1;
    }
    print("identtest: chown-nonroot-denied-ok\n");

    /* --- owned.txt was chown'd to 1000 above, so THIS process (now uid
       1000) really is its owner -- chmod does not require root, only
       ownership, so this must succeed even after the privilege drop. --- */
    if (userChmod("/tmp/identd/owned.txt", 0644) != 0) {
        print("identtest: FAIL owner chmod denied on owned.txt after drop\n");
        return 1;
    }
    if (userStat("/tmp/identd/owned.txt", &st) != 0 || st.mode != 0644) {
        print("identtest: FAIL owned.txt mode not actually changed\n");
        return 1;
    }
    print("identtest: chmod-owner-after-drop-ok\n");

    /* --- but chown is root-only even for your OWN file: real Unix's
       _POSIX_CHOWN_RESTRICTED default means an owner cannot give a file
       away either. owned.txt's uid must stay 1000. --- */
    if (userChown("/tmp/identd/owned.txt", 0) != -1) {
        print("identtest: FAIL owner chown succeeded without being root\n");
        return 1;
    }
    if (userStat("/tmp/identd/owned.txt", &st) != 0 || st.uid != 1000) {
        print("identtest: FAIL owned.txt uid changed by denied chown\n");
        return 1;
    }
    print("identtest: chown-owner-nonroot-denied-ok\n");

    print("identtest: PASS\n");
    return 0;
}
