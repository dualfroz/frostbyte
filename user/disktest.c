#include "ulib.h"
#include "fs.h"

static const char* diskPath = "/dev/disk0";

static void printLong(const char* label, long value)
{
    print(label);
    printDec(value);
    print("\n");
}

static int bytesEqual(const char* a, const char* b, unsigned long n)
{
    for (unsigned long i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

int main(void)
{
    print("disktest: start, pid=");
    printDec(userGetpid());
    print("\n");

    FbStat st;
    if (userStat(diskPath, &st) != 0) {
        print("disktest: stat failed\n");
        return 1;
    }
    printLong("disktest: disk size bytes=", (long)st.size);
    if (st.size < 64 * 1024) {
        print("disktest: FAIL disk suspiciously small\n");
        return 1;
    }

    int fd = (int)userOpen(diskPath, oReadWrite);
    if (fd < 0) {
        print("disktest: open failed\n");
        return 1;
    }

    long end = userLseek(fd, 0, seekEnd);
    printLong("disktest: seekEnd=", end);
    if (end != (long)st.size) {
        print("disktest: FAIL seekEnd does not match stat size\n");
        return 1;
    }

    /* Unaligned write/read spanning a 512-byte sector boundary: byte offset
       50*512 + 500 leaves 12 bytes in sector 50 and spills 8 bytes into
       sector 51, exercising the read-modify-write path in vfs.c blockDevRead
       /blockDevWrite rather than a single aligned sector transfer. */
    const long straddleOffset = 50L * 512 + 500;
    const char* straddlePayload = "ABCDEFGHIJKLMNOPQRST"; /* 20 bytes */
    const unsigned long straddleLen = 20;

    if (userLseek(fd, straddleOffset, seekSet) != straddleOffset) {
        print("disktest: FAIL lseek to straddle offset\n");
        return 1;
    }
    long written = userWrite(fd, straddlePayload, straddleLen);
    printLong("disktest: straddle write bytes=", written);
    if (written != (long)straddleLen) {
        print("disktest: FAIL short straddle write\n");
        return 1;
    }

    if (userLseek(fd, straddleOffset, seekSet) != straddleOffset) {
        print("disktest: FAIL lseek back to straddle offset\n");
        return 1;
    }
    char straddleBack[20];
    long straddleRead = userRead(fd, straddleBack, straddleLen);
    if (straddleRead != (long)straddleLen || !bytesEqual(straddleBack, straddlePayload, straddleLen)) {
        print("disktest: FAIL straddle round-trip mismatch\n");
        return 1;
    }
    print("disktest: straddle round-trip ok\n");

    /* Aligned full-sector write/read at a disjoint offset (sector 55),
       proving the non-read-modify-write path also works. */
    const long alignedOffset = 55L * 512;
    char alignedOut[512];
    for (int i = 0; i < 512; i++)
        alignedOut[i] = (char)('a' + (i % 26));

    if (userLseek(fd, alignedOffset, seekSet) != alignedOffset) {
        print("disktest: FAIL lseek to aligned offset\n");
        return 1;
    }
    if (userWrite(fd, alignedOut, sizeof(alignedOut)) != (long)sizeof(alignedOut)) {
        print("disktest: FAIL aligned sector write\n");
        return 1;
    }
    if (userLseek(fd, alignedOffset, seekSet) != alignedOffset) {
        print("disktest: FAIL lseek back to aligned offset\n");
        return 1;
    }
    char alignedIn[512];
    if (userRead(fd, alignedIn, sizeof(alignedIn)) != (long)sizeof(alignedIn) ||
        !bytesEqual(alignedIn, alignedOut, sizeof(alignedOut))) {
        print("disktest: FAIL aligned sector round-trip mismatch\n");
        return 1;
    }
    print("disktest: aligned sector round-trip ok\n");

    /* Overwrite proves the second write actually lands on disk, not just in
       a stale in-memory copy: reading back must see the newest data. */
    if (userLseek(fd, alignedOffset, seekSet) != alignedOffset)
        return 1;
    char overwrite[512];
    for (int i = 0; i < 512; i++)
        overwrite[i] = (char)('A' + (i % 26));
    userWrite(fd, overwrite, sizeof(overwrite));

    if (userLseek(fd, alignedOffset, seekSet) != alignedOffset)
        return 1;
    char reread[512];
    userRead(fd, reread, sizeof(reread));
    if (!bytesEqual(reread, overwrite, sizeof(overwrite)) || bytesEqual(reread, alignedOut, sizeof(alignedOut))) {
        print("disktest: FAIL overwrite not reflected\n");
        return 1;
    }
    print("disktest: overwrite round-trip ok\n");

    userClose(fd);
    print("disktest: done\n");
    return 0;
}
