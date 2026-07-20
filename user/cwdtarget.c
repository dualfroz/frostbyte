#include "ulib.h"

/*
 * Only ever spawned by cwdtest.c, with no argv at all -- unlike every other
 * *test / *target pair in this kernel (which rendezvous over a shared pipe
 * handle passed as an argv string), this pair's whole point is that the
 * KERNEL itself hands this process its starting directory at creation time
 * (sched.c's threadCreate/threadCreateUser, via the new initialCwd helper
 * that mirrors initialPgid's own "inherit from whoever is currently
 * running" shape for Thread.pgid), never anything cwdtest.c has to pass
 * explicitly.
 *
 * Proves inheritance first, then proves the inherited value is a real,
 * independent COPY rather than shared state: this process's own later
 * chdir() must not reach back into cwdtest.c's Thread.cwd (cwdtest.c's own
 * post-wait getcwd() check is the other half of that proof).
 */

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

    /* Relative path again -- must land at /tmp/cwdb/note, a COMPLETELY
       different file than cwdtest.c's own /tmp/cwda/note despite the
       identical relative name "note", since each process resolves it
       against its own Thread.cwd. */
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
