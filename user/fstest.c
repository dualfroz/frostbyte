#include "ulib.h"
#include "fs.h"

/*
 * Exercises the persistent SFS filesystem (sfs.h/sfs.c) through the exact
 * same syscall path a real userspace program would use: open/write/read/
 * close/mkdir/getdents/unlink on paths under "/mnt", which vfs.c dispatches
 * to sfs.c instead of the kmalloc-based tmpfs tree. Proves the full stack
 * (syscall -> vfs -> sfs -> real block I/O on the same ATA disk /dev/disk0
 * uses) works end to end, not just the in-kernel sfs.c unit tests.
 */

static int bytesEqual(const char* a, const char* b, unsigned long n)
{
    for (unsigned long i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

int main(void)
{
    print("fstest: start, pid=");
    printDec(userGetpid());
    print("\n");

    /* Nested directory creation. */
    if (userMkdir("/mnt/app") != 0) {
        print("fstest: FAIL mkdir /mnt/app\n");
        return 1;
    }
    if (userMkdir("/mnt/app/logs") != 0) {
        print("fstest: FAIL mkdir /mnt/app/logs\n");
        return 1;
    }

    /* Create + write + close, then reopen with a fresh fd to prove the data
       lives past the writer's own fd, not just in that fd's local state. */
    const char* payload = "frostbyte fstest payload line one\n";
    unsigned long payloadLen = stringLength(payload);

    int wfd = (int)userOpen("/mnt/app/logs/boot.log", oReadWrite | oCreate);
    if (wfd < 0) {
        print("fstest: FAIL open for create\n");
        return 1;
    }
    if (userWrite(wfd, payload, payloadLen) != (long)payloadLen) {
        print("fstest: FAIL short write\n");
        return 1;
    }
    userClose(wfd);

    FbStat st;
    if (userStat("/mnt/app/logs/boot.log", &st) != 0 || st.size != payloadLen) {
        print("fstest: FAIL stat size mismatch\n");
        return 1;
    }

    int rfd = (int)userOpen("/mnt/app/logs/boot.log", oReadOnly);
    if (rfd < 0) {
        print("fstest: FAIL reopen\n");
        return 1;
    }
    char back[128];
    long got = userRead(rfd, back, sizeof(back));
    if (got != (long)payloadLen || !bytesEqual(back, payload, payloadLen)) {
        print("fstest: FAIL reopen round-trip mismatch\n");
        return 1;
    }
    userClose(rfd);
    print("fstest: create/write/reopen/read round-trip ok\n");

    /* Append across two separate open() calls: growth must pick up where
       the previous session's data ended. */
    int afd = (int)userOpen("/mnt/app/logs/boot.log", oReadWrite | oAppend);
    if (afd < 0) {
        print("fstest: FAIL open for append\n");
        return 1;
    }
    const char* more = "line two\n";
    unsigned long moreLen = stringLength(more);
    if (userWrite(afd, more, moreLen) != (long)moreLen) {
        print("fstest: FAIL append write\n");
        return 1;
    }
    userClose(afd);

    if (userStat("/mnt/app/logs/boot.log", &st) != 0 || st.size != payloadLen + moreLen) {
        print("fstest: FAIL stat after append\n");
        return 1;
    }
    print("fstest: append across reopen ok\n");

    /* A second, unrelated file in the same directory, to exercise readdir
       against more than one entry. */
    int cfd = (int)userOpen("/mnt/app/logs/counters.txt", oReadWrite | oCreate);
    if (cfd < 0) {
        print("fstest: FAIL create second file\n");
        return 1;
    }
    userWrite(cfd, "0", 1);
    userClose(cfd);

    int dfd = (int)userOpen("/mnt/app/logs", oReadOnly);
    if (dfd < 0) {
        print("fstest: FAIL open dir for listing\n");
        return 1;
    }
    FbDirent entries[8];
    long entCount = userGetdents(dfd, entries, sizeof(entries));
    userClose(dfd);
    if (entCount != 2 * (long)sizeof(FbDirent)) {
        print("fstest: FAIL readdir entry count\n");
        return 1;
    }
    int sawBoot = 0, sawCounters = 0;
    for (int i = 0; i < 2; i++) {
        if (entries[i].type != fsTypeFile) {
            print("fstest: FAIL readdir entry type\n");
            return 1;
        }
        if (bytesEqual(entries[i].name, "boot.log", 9))
            sawBoot = 1;
        if (bytesEqual(entries[i].name, "counters.txt", 13))
            sawCounters = 1;
    }
    if (!sawBoot || !sawCounters) {
        print("fstest: FAIL readdir missing an expected entry\n");
        return 1;
    }
    print("fstest: readdir lists both files ok\n");

    /* Unlink one, verify it is gone and its sibling survives. */
    if (userUnlink("/mnt/app/logs/counters.txt") != 0) {
        print("fstest: FAIL unlink counters.txt\n");
        return 1;
    }
    if (userStat("/mnt/app/logs/counters.txt", &st) == 0) {
        print("fstest: FAIL unlinked file still stat-able\n");
        return 1;
    }
    if (userStat("/mnt/app/logs/boot.log", &st) != 0) {
        print("fstest: FAIL sibling disappeared after unlink\n");
        return 1;
    }
    print("fstest: unlink ok, sibling survives\n");

    print("fstest: done\n");
    return 0;
}
