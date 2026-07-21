#include "ulib.h"

/*
 * Sibling of namedreader.c -- see its header comment for the full picture.
 * Sleeps well past when namedreader.c (spawned moments earlier, at boot)
 * will already be parked in a blocking read() on the still-empty named
 * pipe, so the write below has to actually wake it back up (schedWakeChannel
 * via pipeWrite, pipe.c) instead of the reader simply finding data already
 * waiting -- same proof pattern fifowriter.c uses for the handle-based
 * shared pipe, now for a path-based one neither side had to be told about
 * by a common parent.
 */

#define namedPipePath "/tmp/rendezvous"
#define namedWriterDelayMs 200

static const char* payload = "hello via named pipe";

int main(void)
{
    print("namedwriter: start, pid=");
    printDec(userGetpid());
    print("\n");

    userSleep(namedWriterDelayMs);

    long fd = userOpen(namedPipePath, oWriteOnly);
    if (fd < 0) {
        print("namedwriter: FAIL open\n");
        return 1;
    }

    long written = userWrite((int)fd, payload, stringLength(payload));
    if (written != (long)stringLength(payload)) {
        print("namedwriter: FAIL short/failed write\n");
        return 1;
    }
    print("namedwriter: wrote payload\n");

    userClose((int)fd);
    return 0;
}
