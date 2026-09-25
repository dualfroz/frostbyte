#include "ulib.h"
#include "fs.h"

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

    if (copyFile("/bin/identfalse", "/tmp/identd/prog") != 0) {
        print("identtest: FAIL copy /bin/identfalse to prog\n");
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

    if (userSetuid(1000) != 0) {
        print("identtest: FAIL root could not setuid(1000)\n");
        return 1;
    }
    if (userGetuid() != 1000) {
        print("identtest: FAIL uid did not actually change to 1000\n");
        return 1;
    }
    print("identtest: drop-to-1000-ok\n");

    if (userSetuid(0) != -1) {
        print("identtest: FAIL non-root regained root via setuid\n");
        return 1;
    }
    if (userGetuid() != 1000) {
        print("identtest: FAIL uid changed despite rejected setuid\n");
        return 1;
    }
    print("identtest: cannot-regain-root-ok\n");

    if (userSetuid(1000) != 0) {
        print("identtest: FAIL setuid to own current uid was rejected\n");
        return 1;
    }
    print("identtest: setuid-self-noop-ok\n");

    if (userOpen("/tmp/identd/secret.txt", oWriteOnly) >= 0) {
        print("identtest: FAIL write succeeded after dropping ownership\n");
        return 1;
    }
    print("identtest: write-denied-non-owner-ok\n");

    if (userSpawn("/tmp/identd/prog", 0) >= 0) {
        print("identtest: FAIL spawn succeeded after dropping ownership\n");
        return 1;
    }
    print("identtest: exec-denied-non-owner-ok\n");

    if (userChmod("/tmp/identd/secret.txt", 0666) != -1) {
        print("identtest: FAIL non-owner chmod succeeded on secret.txt\n");
        return 1;
    }
    if (userStat("/tmp/identd/secret.txt", &st) != 0 || st.mode != 0600) {
        print("identtest: FAIL secret.txt mode changed by denied chmod\n");
        return 1;
    }
    print("identtest: chmod-nonowner-denied-ok\n");

    if (userChown("/tmp/identd/secret.txt", 1000) != -1) {
        print("identtest: FAIL non-root chown succeeded on secret.txt\n");
        return 1;
    }
    if (userStat("/tmp/identd/secret.txt", &st) != 0 || st.uid != 0) {
        print("identtest: FAIL secret.txt uid changed by denied chown\n");
        return 1;
    }
    print("identtest: chown-nonroot-denied-ok\n");

    if (userChmod("/tmp/identd/owned.txt", 0644) != 0) {
        print("identtest: FAIL owner chmod denied on owned.txt after drop\n");
        return 1;
    }
    if (userStat("/tmp/identd/owned.txt", &st) != 0 || st.mode != 0644) {
        print("identtest: FAIL owned.txt mode not actually changed\n");
        return 1;
    }
    print("identtest: chmod-owner-after-drop-ok\n");

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
