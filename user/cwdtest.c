#include "ulib.h"

/*
 * Boot-time driver for real per-process working directories (sched.h's new
 * Thread.cwd, vfs.h's new vfsChdir/vfsGetcwd/vfsResolvePath). Before this
 * fala the only "current directory" concept anywhere in this kernel was a
 * single static local inside shell.c, shared by nothing else -- a spawned
 * user process calling open()/mkdir()/etc with a RELATIVE path had never
 * actually worked, since vfs.c's lookupNode/sfsLookup always resolved
 * relative to the filesystem root, never to any per-process notion of
 * "here".
 *
 * This proves three things a single shared global cannot: (1) THIS
 * process's own chdir() makes its OWN later relative opens resolve there,
 * (2) cwdtarget, spawned AFTER that chdir(), inherits the new directory at
 * creation time (real fork()'s own cwd-copy semantics, sched.c's new
 * initialCwd mirroring initialPgid's own shape exactly), and (3)
 * cwdtarget's own later chdir() does NOT reach back and change THIS
 * process's -- each Thread owns an independent buffer, never a pointer to
 * shared state. No pipes, signals, or injected keystrokes anywhere in this
 * pair: every step is ordered by a real userWait, so unlike most of this
 * kernel's other live demos this one has no scheduling-timing race to flake
 * on at all.
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

    /* Relative path -- no leading '/' -- must resolve against the cwd just
       set above, landing at /tmp/cwda/note, not the filesystem root. */
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

    /* cwdtarget's own chdir("/tmp/cwdb") must never have reached back into
       THIS process's Thread.cwd -- each is an independent buffer, not a
       pointer to shared state (unlike shell.c's OLD single static global
       this subsystem replaces). */
    len = userGetcwd(cwdBuf, sizeof(cwdBuf));
    if (len < 0 || !streq(cwdBuf, "/tmp/cwda")) {
        print("cwdtest: FAIL own cwd changed after child exited, got: ");
        print(len < 0 ? "(error)" : cwdBuf);
        print("\n");
        return 1;
    }
    print("cwdtest: parent-cwd-unaffected-ok\n");

    /* Re-read using an absolute path this time -- still resolves to the
       exact same file the earlier relative open created, and must still
       hold the ORIGINAL payload, untouched by anything cwdtarget did in
       its own, completely separate directory. */
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
