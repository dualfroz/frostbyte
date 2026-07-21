#include "ulib.h"

/*
 * Right half of shell.c's new `cmd1 | cmd2` pipeline syntax's live demo --
 * see pipesrc.c's own header comment for the full picture. jobctlSpawnPiped
 * bakes the SAME pipe's read end into this process's own fd 0 before it
 * ever runs, so the blocking userRead below has to genuinely retry across
 * timer ticks (fd 0's pipe end is flagged fdFlagBlocking, see
 * vfsPipeAttachInto) until pipesrc.c's own write actually lands, exactly
 * the same real cross-process wake this kernel's every other pipe/fifo
 * demo already proves (schedWakeChannel via pipeWrite, pipe.c) -- never a
 * coincidence of scheduling order (jobctlSpawnPiped installs pipesrc.c's
 * write end into the pipe's writer count the moment IT is created, well
 * before it ever runs, so this process can never see a false "no writer,
 * EOF" even if it is scheduled to run first).
 *
 * Reads in a loop until a real EOF (0 return, once pipesrc.c has both
 * written and exited -- see that file's own comment on why exit matters
 * here) rather than trusting a single userRead call to receive the whole
 * payload in one shot, the honest way a real pipe reader has to work.
 *
 * Unlike pipesrc.c, this process's own fd 1 (stdout) is NOT part of the
 * pipeline (only fd 0 was redirected) -- print()/printDec() (ulib.h) reach
 * the real console exactly like any unpiped demo, so the needle below is
 * exactly as observable to ci-test.sh as every other one.
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
    print("pipesink: blocking on read, waiting for pipesrc\n");

    char buffer[64];
    unsigned long total = 0;
    for (;;) {
        long got = userRead(0, buffer + total, sizeof(buffer) - 1 - total);
        if (got < 0) {
            print("pipesink: FAIL read\n");
            return 1;
        }
        if (got == 0)
            break;
        total += (unsigned long)got;
        if (total >= sizeof(buffer) - 1)
            break;
    }
    buffer[total] = 0;

    print("pipesink: got: ");
    print(buffer);
    print("\n");

    int ok = streq(buffer, "pipeline-payload-ok");
    print(ok ? "pipesink: payload-ok\n" : "pipesink: payload-FAIL\n");

    return ok ? 0 : 1;
}
