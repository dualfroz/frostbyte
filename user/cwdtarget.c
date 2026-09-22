#include "ulib.h"

static int streq(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(void)
{
    print("cwdtarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    char cwdBuf[64];
    long len = userGetcwd(cwdBuf, sizeof(cwdBuf));
    if (len < 0 || !streq(cwdBuf, "/tmp/cwda")) {
        print("cwdtarget: FAIL inherited cwd, got: ");
        print(len < 0 ? "(error)" : cwdBuf);
        print("\n");
        return 1;
    }
    print("cwdtarget: inherited-cwd-ok\n");

    if (userChdir("/tmp/cwdb") != 0) {
        print("cwdtarget: FAIL chdir /tmp/cwdb\n");
        return 1;
    }
    len = userGetcwd(cwdBuf, sizeof(cwdBuf));
    if (len < 0 || !streq(cwdBuf, "/tmp/cwdb")) {
        print("cwdtarget: FAIL getcwd after own chdir, got: ");
        print(len < 0 ? "(error)" : cwdBuf);
        print("\n");
        return 1;
    }

    int fd = (int)userOpen("note", oCreate | oWriteOnly | oTrunc);
    if (fd < 0) {
        print("cwdtarget: FAIL create relative note in cwdb\n");
        return 1;
    }
    const char* payload = "cwdb-payload";
    unsigned long payloadLen = stringLength(payload);
    if (userWrite(fd, payload, payloadLen) != (long)payloadLen) {
        print("cwdtarget: FAIL write relative note in cwdb\n");
        return 1;
    }
    userClose(fd);
    print("cwdtarget: relative-write-cwdb-ok\n");

    return 0;
}
