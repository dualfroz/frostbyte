#include "ulib.h"

/*
 * Only ever spawned by polltest.c, with the shared pipe's handle number as
 * argv[1] (decToString'd -- see polltest.c, mirrors fifomaster.c's own
 * handle-passing convention). Sleeps well past when polltest.c will
 * already be parked inside its own userPoll(..., -1) call, so the write
 * below has to actually wake it back up (schedWakeChannel(schedPollChannel()),
 * pipeWrite in pipe.c) instead of polltest simply finding the pipe already
 * readable -- proof the block was real, not a lucky race, the same shape
 * fifowriter.c already proves for a plain blocking read().
 */

/* Comfortably longer than polltest.c's own phase-2 bounded-timeout poll
   (pollTimeoutMs, polltest.c) -- pollwriter is spawned immediately, at the
   very start of polltest.c (mirroring fifomaster.c's own immediate double-
   spawn), so by the time polltest reaches its phase-3 blocking multi-fd
   poll, phase 2's own wait has already used up part of this delay; the
   margin here guarantees phase 3 still genuinely has to block and be woken
   by the write below, not race a write that already happened. */
#define pollWriterDelayMs 500

static const char* payload = "poll payload";

int main(int argc, char** argv)
{
    if (argc != 2) {
        print("pollwriter: FAIL expected one argv (pipe handle)\n");
        return 1;
    }

    long handle = parseDec(argv[1]);
    long fd = userPipeAttach(handle, 1);
    if (fd < 0) {
        print("pollwriter: FAIL attach\n");
        return 1;
    }

    print("pollwriter: attached, sleeping before write\n");
    userSleep(pollWriterDelayMs);

    long written = userWrite((int)fd, payload, stringLength(payload));
    if (written != (long)stringLength(payload)) {
        print("pollwriter: FAIL short/failed write\n");
        return 1;
    }
    print("pollwriter: wrote payload\n");

    userClose((int)fd);
    return 0;
}
