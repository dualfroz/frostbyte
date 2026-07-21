#include "ulib.h"

/*
 * Left half of shell.c's new `cmd1 | cmd2` pipeline syntax's live demo.
 * jobctlSpawnPiped (jobctl.h) bakes a real pipe end into this process's own
 * fd 1 BEFORE it ever runs a single instruction (vfsPipeAttachInto, vfs.h)
 * -- from this program's own point of view it writes its stdout exactly
 * like any well-behaved Unix filter, with no idea a pipe is on the other
 * end instead of the real console. fd 2 (stderr) is deliberately left
 * alone by the pipeline setup (only fd 0/fd 1 are ever redirected, matching
 * a real shell), so the diagnostic lines below go straight to the real
 * console even though fd 1's own payload does not -- writing them through
 * fd 1 instead (print()/printDec(), ulib.h, both hardcoded to fd 1) would
 * corrupt the exact payload pipesink.c compares against.
 *
 * Deliberately writes once and exits immediately: pipesink.c's own
 * blocking read has to notice the resulting EOF (this process's own exit
 * reclaims its fd 1, dropping the pipe's writer count to 0 -- see
 * sched.c's reclaimOpenFds) the same way every other pipe/fifo EOF demo in
 * this kernel already does, not just receive the payload.
 */

#define pipelinePayload "pipeline-payload-ok"

static void printErr(const char* text)
{
    userWrite(2, text, stringLength(text));
}

int main(void)
{
    printErr("pipesrc: writing payload to stdout (pipe)\n");

    unsigned long length = stringLength(pipelinePayload);
    long written = userWrite(1, pipelinePayload, length);
    if (written != (long)length) {
        printErr("pipesrc: FAIL short/failed write\n");
        return 1;
    }

    printErr("pipesrc: wrote payload\n");
    return 0;
}
