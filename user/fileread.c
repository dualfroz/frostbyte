#include "ulib.h"
#include "fs.h"

static const char* motdPath = "/etc/motd.txt";
static const char* notePath = "/tmp/note.txt";

int main(void)
{
    print("fileread: start, pid=");
    printDec(userGetpid());
    print("\n");

    FbStat st;
    if (userStat(motdPath, &st) == 0) {
        print("fileread: stat ");
        print(motdPath);
        print(" size=");
        printDec((long)st.size);
        print("\n");
    } else {
        print("fileread: stat failed\n");
    }

    int fd = (int)userOpen(motdPath, oReadOnly);
    if (fd < 0) {
        print("fileread: open failed\n");
        return 1;
    }

    print("fileread: --- begin ");
    print(motdPath);
    print(" ---\n");
    char buffer[128];
    long n;
    while ((n = userRead(fd, buffer, sizeof(buffer))) > 0)
        userWrite(1, buffer, (unsigned long)n);
    print("fileread: --- end ---\n");

    userLseek(fd, 0, seekSet);
    n = userRead(fd, buffer, 16);
    print("fileread: lseek reread first bytes: ");
    userWrite(1, buffer, (unsigned long)n);
    print("\n");
    userClose(fd);

    int wfd = (int)userOpen(notePath, oReadWrite | oCreate | oTrunc);
    if (wfd < 0) {
        print("fileread: tmpfs open failed\n");
        return 2;
    }
    const char* payload = "frostbyte-tmpfs-rw-ok\n";
    userWrite(wfd, payload, stringLength(payload));
    userLseek(wfd, 0, seekSet);
    char readback[64];
    long rn = userRead(wfd, readback, sizeof(readback));
    print("fileread: tmpfs read back: ");
    userWrite(1, readback, (unsigned long)rn);
    userClose(wfd);

    print("fileread: listing /etc via getdents:\n");
    int dfd = (int)userOpen("/etc", oReadOnly);
    if (dfd >= 0) {
        FbDirent entries[8];
        long bytes = userGetdents(dfd, entries, sizeof(entries));
        long records = bytes / (long)sizeof(FbDirent);
        for (long i = 0; i < records; i++) {
            print("  ");
            print(entries[i].name);
            print(entries[i].type == fsTypeDir ? "/" : "");
            print("\n");
        }
        userClose(dfd);
    }

    print("fileread: done\n");
    return 0;
}
