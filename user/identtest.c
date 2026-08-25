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

