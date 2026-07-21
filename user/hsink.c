#include "ulib.h"

/*
 * Right half of shell.c's new `cmd1 <<< TEXT | cmd2` here-string-into-
 * pipeline live demo -- see hcat.c's own header comment for the full
 * picture. jobctlSpawnPiped bakes the pipeline's shared pipe's read end
 * into this process's own fd 0 exactly like pipesink.c's, so the blocking
 * userRead below has to genuinely retry across timer ticks until hcat.c's
 * own forwarded write actually lands. Compares the received bytes
 * byte-for-byte against the exact text kmain.c's driver thread types after
 * `<<<` plus the single trailing newline jobctlSpawnPipedHereString always
 * appends (matching a real shell's own `<<<` behavior) -- unlike hcat.c,
 * this process's own fd 1 (stdout) is NOT part of the pipeline, so
 * print()/printDec() (ulib.h) reach the real console exactly like
 * pipesink.c's own needle.
 */
#define hsinkPayload "hpipe-ok\n"

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
    print("hsink: blocking on read, waiting for hcat\n");

    char buffer[32];
    unsigned long total = 0;
    for (;;) {
        long got = userRead(0, buffer + total, sizeof(buffer) - 1 - total);
        if (got < 0) {
            print("hsink: FAIL read\n");
            return 1;
        }
        if (got == 0)
            break;
        total += (unsigned long)got;
        if (total >= sizeof(buffer) - 1)
            break;
    }
    buffer[total] = 0;

    print("hsink: got: ");
    print(buffer);

    int ok = streq(buffer, hsinkPayload);
    print(ok ? "hsink: payload-ok\n" : "hsink: payload-FAIL\n");

    return ok ? 0 : 1;
}
