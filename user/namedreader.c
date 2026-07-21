#include "ulib.h"

/*
 * Boot-spawned independently of namedwriter.c (see kmain.c's spawnUser
 * calls) -- unlike fiforeader.c/fifowriter.c, which fifomaster.c spawns as
 * its own children and hands a shared pipe.h handle to via argv, this
 * process and namedwriter.c have no relationship at all beyond both being
 * launched at boot: neither one's pid, argv, or fd table ever mentions the
 * other. The only thing they share is agreeing on the path "/tmp/rendezvous"
 * (mkfifo'd by kmain.c before either is spawned) -- proving vfsMkfifo/
 * vfsOpen (vfs.h) really do let two unrelated processes rendezvous purely
 * through the filesystem namespace, the gap FALA 11 explicitly left open.
 * Opens for read immediately, well before namedwriter.c's deliberate sleep
 * elapses, so the blocking read() below has to genuinely park across many
 * timer ticks and be woken later -- not just win a race against data that
 * was already sitting in the pipe.
 */

#define namedPipePath "/tmp/rendezvous"

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
    print("namedreader: start, pid=");
    printDec(userGetpid());
    print("\n");

    long fd = userOpen(namedPipePath, oReadOnly);
    if (fd < 0) {
        print("namedreader: FAIL open\n");
        return 1;
    }

    print("namedreader: opened, blocking on read\n");
    char buffer[64];
    long got = userRead((int)fd, buffer, sizeof(buffer) - 1);
    if (got <= 0) {
        print("namedreader: FAIL read\n");
        return 1;
    }
    buffer[got] = 0;

    print("namedreader: got: ");
    print(buffer);
    print("\n");

    int ok = streq(buffer, "hello via named pipe");
    print(ok ? "namedreader: payload-ok\n" : "namedreader: payload-FAIL\n");

    userClose((int)fd);
    return ok ? 0 : 1;
}
