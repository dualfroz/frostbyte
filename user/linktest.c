#include "ulib.h"
#include "fs.h"

static int bytesEqual(const char* a, const char* b, unsigned long n)
{
    for (unsigned long i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

int main(void)
{
    print("linktest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userMkdir("/mnt/linktest") != 0) {
        print("linktest: FAIL mkdir /mnt/linktest\n");
        return 1;
    }

    const char* payload = "frostbyte linktest payload\n";
    unsigned long payloadLen = stringLength(payload);

    int wfd = (int)userOpen("/mnt/linktest/real.txt", oReadWrite | oCreate);
    if (wfd < 0) {
        print("linktest: FAIL create real.txt\n");
        return 1;
    }
    if (userWrite(wfd, payload, payloadLen) != (long)payloadLen) {
        print("linktest: FAIL write real.txt\n");
        return 1;
    }
    userClose(wfd);

    if (userSymlink("/linktest/real.txt", "/mnt/linktest/link.txt") != 0) {
        print("linktest: FAIL symlink create\n");
        return 1;
    }

    int lfd = (int)userOpen("/mnt/linktest/link.txt", oReadOnly);
    if (lfd < 0) {
        print("linktest: FAIL open through symlink\n");
        return 1;
    }
    char back[64];
    long got = userRead(lfd, back, sizeof(back));
    userClose(lfd);
    if (got != (long)payloadLen || !bytesEqual(back, payload, payloadLen)) {
        print("linktest: FAIL read through symlink mismatch\n");
        return 1;
    }
    print("linktest: open/read transparently follows symlink ok\n");

    FbStat st;
    if (userStat("/mnt/linktest/link.txt", &st) != 0 || st.size != payloadLen) {
        print("linktest: FAIL stat through symlink\n");
        return 1;
    }
    print("linktest: stat follows symlink ok\n");

    char targetBuf[64];
    long targetLen = userReadlink("/mnt/linktest/link.txt", targetBuf, sizeof(targetBuf));
    const char* expectedTarget = "/linktest/real.txt";
    unsigned long expectedTargetLen = stringLength(expectedTarget);
    if (targetLen != (long)expectedTargetLen ||
        !bytesEqual(targetBuf, expectedTarget, expectedTargetLen)) {
        print("linktest: FAIL readlink target mismatch\n");
        return 1;
    }
    print("linktest: readlink reports target string ok\n");

    if (userReadlink("/mnt/linktest/real.txt", targetBuf, sizeof(targetBuf)) >= 0) {
        print("linktest: FAIL readlink on non-symlink should fail\n");
        return 1;
    }
    print("linktest: readlink rejects non-symlink ok\n");

    if (userLink("/mnt/linktest/real.txt", "/mnt/linktest/alias.txt") != 0) {
        print("linktest: FAIL hardlink create\n");
        return 1;
    }
    int afd = (int)userOpen("/mnt/linktest/alias.txt", oReadOnly);
    if (afd < 0) {
        print("linktest: FAIL open alias.txt\n");
        return 1;
    }
    got = userRead(afd, back, sizeof(back));
    userClose(afd);
    if (got != (long)payloadLen || !bytesEqual(back, payload, payloadLen)) {
        print("linktest: FAIL alias.txt content mismatch\n");
        return 1;
    }
    print("linktest: hardlink shares data ok\n");

    if (userLink("/mnt/linktest", "/mnt/linktest2") == 0) {
        print("linktest: FAIL hardlinking a directory should be rejected\n");
        return 1;
    }
    print("linktest: hardlink rejects directory ok\n");

    if (userUnlink("/mnt/linktest/real.txt") != 0) {
        print("linktest: FAIL unlink real.txt\n");
        return 1;
    }
    afd = (int)userOpen("/mnt/linktest/alias.txt", oReadOnly);
    if (afd < 0) {
        print("linktest: FAIL alias.txt gone after unlinking real.txt\n");
        return 1;
    }
    got = userRead(afd, back, sizeof(back));
    userClose(afd);
    if (got != (long)payloadLen || !bytesEqual(back, payload, payloadLen)) {
        print("linktest: FAIL alias.txt data lost after unlinking real.txt\n");
        return 1;
    }
    print("linktest: alias survives unlink of original ok\n");

    if (userSymlink("/linktest/does-not-exist.txt", "/mnt/linktest/dangling.txt") != 0) {
        print("linktest: FAIL dangling symlink create\n");
        return 1;
    }
    if (userOpen("/mnt/linktest/dangling.txt", oReadOnly) >= 0) {
        print("linktest: FAIL open through dangling symlink should fail\n");
        return 1;
    }
    print("linktest: dangling symlink open fails cleanly ok\n");

    print("linktest: done\n");
    return 0;
}
