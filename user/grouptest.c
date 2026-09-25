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
    print("grouptest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userGetuid() != 0 || userGetgid() != 0) {
        print("grouptest: FAIL boot-time uid/gid is not 0\n");
        return 1;
    }
    print("grouptest: root-by-default-ok\n");

    if (userMkdir("/tmp/groupd") != 0) {
        print("grouptest: FAIL mkdir /tmp/groupd\n");
        return 1;
    }

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

    if (copyFile("/bin/groupfalse", "/tmp/groupd/progOk") != 0 ||
        copyFile("/bin/groupfalse", "/tmp/groupd/progDenied") != 0) {
        print("grouptest: FAIL copy /bin/groupfalse\n");
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

    if (userSetgid(7000) != 0) {
        print("grouptest: FAIL root could not setgid(7000)\n");
        return 1;
    }
    if (userGetgid() != 7000) {
        print("grouptest: FAIL gid did not actually change to 7000\n");
        return 1;
    }
    print("grouptest: setgid-root-arbitrary-ok\n");

    if (userSetuid(6000) != 0) {
        print("grouptest: FAIL root could not setuid(6000)\n");
        return 1;
    }
    if (userGetuid() != 6000 || userGetgid() != 7000) {
        print("grouptest: FAIL identity after drop is not (6000,7000)\n");
        return 1;
    }
    print("grouptest: drop-to-6000-7000-ok\n");

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

    if (userOpen("/tmp/groupd/mismatch", oReadOnly) >= 0) {
        print("grouptest: FAIL non-member read succeeded via wrong branch\n");
        return 1;
    }
    print("grouptest: other-fallback-denied-ok\n");

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
