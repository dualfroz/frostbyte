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
    print("permtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userMkdir("/tmp/permd") != 0) {
        print("permtest: FAIL mkdir /tmp/permd\n");
        return 1;
    }

    int fd = (int)userOpen("/tmp/permd/data.txt", oReadWrite | oCreate);
    if (fd < 0) {
        print("permtest: FAIL create data.txt\n");
        return 1;
    }
    if (userWrite(fd, "hi", 2) != 2) {
        print("permtest: FAIL initial write\n");
        return 1;
    }
    userClose(fd);

    FbStat st;
    if (userStat("/tmp/permd/data.txt", &st) != 0 || st.mode != 0644) {
        print("permtest: FAIL default mode not 0644\n");
        return 1;
    }
    print("permtest: default-mode-ok\n");

    if (userChmod("/tmp/permd/data.txt", 0444) != 0) {
        print("permtest: FAIL chmod to read-only\n");
        return 1;
    }
    if (userStat("/tmp/permd/data.txt", &st) != 0 || st.mode != 0444) {
        print("permtest: FAIL stat after chmod 0444\n");
        return 1;
    }
    if (userOpen("/tmp/permd/data.txt", oWriteOnly) >= 0) {
        print("permtest: FAIL write-only open succeeded on a read-only file\n");
        return 1;
    }
    int rfd = (int)userOpen("/tmp/permd/data.txt", oReadOnly);
    if (rfd < 0) {
        print("permtest: FAIL read-only open failed on a readable file\n");
        return 1;
    }
    char rdbuf[4];
    if (userRead(rfd, rdbuf, 2) != 2) {
        print("permtest: FAIL read from read-only file\n");
        return 1;
    }
    userClose(rfd);
    print("permtest: write-denied-ok\n");

    if (userChmod("/tmp/permd/data.txt", 0644) != 0) {
        print("permtest: FAIL chmod back to 0644\n");
        return 1;
    }
    int wfd = (int)userOpen("/tmp/permd/data.txt", oWriteOnly | oAppend);
    if (wfd < 0) {
        print("permtest: FAIL write open failed after chmod restored write bit\n");
        return 1;
    }
    if (userWrite(wfd, "!", 1) != 1) {
        print("permtest: FAIL write after chmod restored\n");
        return 1;
    }
    userClose(wfd);
    print("permtest: write-restored-ok\n");

    if (copyFile("/bin/permfalse", "/tmp/permd/prog") != 0) {
        print("permtest: FAIL copy /bin/permfalse to prog\n");
        return 1;
    }
    if (userStat("/tmp/permd/prog", &st) != 0 || (st.mode & 0100) != 0) {
        print("permtest: FAIL fresh copy already executable\n");
        return 1;
    }
    print("permtest: copy-not-executable-ok\n");

    if (userSpawn("/tmp/permd/prog", 0) >= 0) {
        print("permtest: FAIL spawn succeeded without exec bit\n");
        return 1;
    }
    print("permtest: exec-denied-ok\n");

    if (userChmod("/tmp/permd/prog", 0755) != 0) {
        print("permtest: FAIL chmod +x\n");
        return 1;
    }
    long pid = userSpawn("/tmp/permd/prog", 0);
    if (pid < 0) {
        print("permtest: FAIL spawn after chmod +x\n");
        return 1;
    }
    long status = userWait(pid);
    if (status != 1) {
        print("permtest: FAIL spawned copy did not exit 1\n");
        return 1;
    }
    print("permtest: exec-after-chmod-ok\n");

    print("permtest: PASS\n");
    return 0;
}
