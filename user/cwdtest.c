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
    print("cwdtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    userMkdir("/tmp/cwda");
    userMkdir("/tmp/cwdb");

    if (userChdir("/tmp/cwda") != 0) {
        print("cwdtest: FAIL chdir /tmp/cwda\n");
        return 1;
    }

    char cwdBuf[64];
    long len = userGetcwd(cwdBuf, sizeof(cwdBuf));
    if (len < 0 || !streq(cwdBuf, "/tmp/cwda")) {
        print("cwdtest: FAIL getcwd after chdir, got: ");
        print(len < 0 ? "(error)" : cwdBuf);
        print("\n");
        return 1;
    }
    print("cwdtest: chdir-cwda-ok\n");

    int fd = (int)userOpen("note", oCreate | oWriteOnly | oTrunc);
    if (fd < 0) {
        print("cwdtest: FAIL create relative note in cwda\n");
        return 1;
    }
    const char* cwdaPayload = "cwda-payload";
    unsigned long cwdaLen = stringLength(cwdaPayload);
    if (userWrite(fd, cwdaPayload, cwdaLen) != (long)cwdaLen) {
        print("cwdtest: FAIL write relative note in cwda\n");
        return 1;
    }
    userClose(fd);
    print("cwdtest: relative-write-cwda-ok\n");

    long target = userSpawn("/bin/cwdtarget", 0);
    if (target < 0) {
        print("cwdtest: FAIL spawn cwdtarget\n");
        return 1;
    }

    long status = userWait(target);
    print("cwdtest: cwdtarget exit status=");
    printDec(status);
    print("\n");
    if (status != 0) {
        print("cwdtest: FAIL cwdtarget did not exit cleanly\n");
        return 1;
    }

    len = userGetcwd(cwdBuf, sizeof(cwdBuf));
    if (len < 0 || !streq(cwdBuf, "/tmp/cwda")) {
        print("cwdtest: FAIL own cwd changed after child exited, got: ");
        print(len < 0 ? "(error)" : cwdBuf);
        print("\n");
        return 1;
    }
    print("cwdtest: parent-cwd-unaffected-ok\n");

    char readBuf[32];
    fd = (int)userOpen("/tmp/cwda/note", oReadOnly);
    if (fd < 0) {
        print("cwdtest: FAIL reopen /tmp/cwda/note\n");
        return 1;
    }
    long got = userRead(fd, readBuf, sizeof(readBuf) - 1);
    userClose(fd);
    if (got < 0) {
        print("cwdtest: FAIL read /tmp/cwda/note\n");
        return 1;
    }
    readBuf[got] = 0;
    if (!streq(readBuf, cwdaPayload)) {
        print("cwdtest: FAIL /tmp/cwda/note payload mismatch, got: ");
        print(readBuf);
        print("\n");
        return 1;
    }

    fd = (int)userOpen("/tmp/cwdb/note", oReadOnly);
    if (fd < 0) {
        print("cwdtest: FAIL reopen /tmp/cwdb/note\n");
        return 1;
    }
    got = userRead(fd, readBuf, sizeof(readBuf) - 1);
    userClose(fd);
    if (got < 0) {
        print("cwdtest: FAIL read /tmp/cwdb/note\n");
        return 1;
    }
    readBuf[got] = 0;
    if (!streq(readBuf, "cwdb-payload")) {
        print("cwdtest: FAIL /tmp/cwdb/note payload mismatch, got: ");
        print(readBuf);
        print("\n");
        return 1;
    }

    print("cwdtest: isolation-verified-ok\n");
    return 0;
}
