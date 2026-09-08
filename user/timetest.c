#include "ulib.h"
#include "fs.h"

/*
 * Exercises SFS's new real wall-clock inode timestamps (sfs.h's
 * sfsMtimeOf/sfsCtimeOf) through the exact same syscall path a real
 * userspace program would use (userOpen/userWrite/userStat/userLink on
 * "/mnt", like fstest.c/linktest.c already do for size/content), proving the
 * full stack -- syscall -> vfs -> sfs -> the on-disk inode's mtime/ctime
 * fields, stamped from the real CMOS RTC (rtc.c) -- works end to end, not
 * just the in-kernel sfs.c unit tests. Fully synchronous like nicetest.c/
 * date.c: no injected keystrokes, no shared shell channel, so there is no
 * scheduling-timing race to flake on regardless of host load.
 */

int main(void)
{
    print("timetest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userMkdir("/mnt/timetest") != 0) {
        print("timetest: FAIL mkdir /mnt/timetest\n");
        return 1;
    }

    long before = userTime();
    int fd = (int)userOpen("/mnt/timetest/data.txt", oReadWrite | oCreate);
    long after = userTime();
    if (fd < 0) {
        print("timetest: FAIL open for create\n");
        return 1;
    }

    FbStat st;
    if (userStat("/mnt/timetest/data.txt", &st) != 0) {
        print("timetest: FAIL stat after create\n");
        return 1;
    }
    long createdMtime = (long)st.mtime;
    long createdCtime = (long)st.ctime;
    if (createdMtime != createdCtime || createdMtime < before || createdMtime > after) {
        print("timetest: FAIL creation timestamp out of range\n");
        return 1;
    }
    print("timetest: create-stamped-ok\n");

    const char* payload = "timetest payload\n";
    unsigned long payloadLen = stringLength(payload);
    if (userWrite(fd, payload, payloadLen) != (long)payloadLen) {
        print("timetest: FAIL write\n");
        return 1;
    }
    userClose(fd);

    if (userStat("/mnt/timetest/data.txt", &st) != 0) {
        print("timetest: FAIL stat after write\n");
        return 1;
    }
    if ((long)st.mtime != (long)st.ctime || (long)st.mtime < createdMtime) {
        print("timetest: FAIL write did not bump mtime\n");
        return 1;
    }
    print("timetest: write-stamped-ok\n");
    long mtimeAfterWrite = (long)st.mtime;

    if (userLink("/mnt/timetest/data.txt", "/mnt/timetest/alias.txt") != 0) {
        print("timetest: FAIL hardlink\n");
        return 1;
    }

    FbStat aliasSt;
    if (userStat("/mnt/timetest/alias.txt", &aliasSt) != 0) {
        print("timetest: FAIL stat alias\n");
        return 1;
    }
    if ((long)aliasSt.mtime != mtimeAfterWrite) {
        print("timetest: FAIL hardlink does not share mtime\n");
        return 1;
    }
    if ((long)aliasSt.ctime < mtimeAfterWrite) {
        print("timetest: FAIL hardlink ctime went backwards\n");
        return 1;
    }
    print("timetest: hardlink-shares-mtime-ok\n");

    print("timetest: PASS\n");
    return 0;
}
