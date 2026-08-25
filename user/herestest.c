#include "ulib.h"

/*
 * Live ring-3 proof for shell.c's new `cmd <<< TEXT` here-string syntax
 * (jobctl.c's jobctlSpawnForegroundHereString/jobctlSpawnHereStringPiped):
 * from this program's own point of view fd 0 is just an ordinary blocking
 * pipe read end, exactly like pipesink.c's -- it has no idea the other end
 * was ever the SHELL ITSELF writing literal typed text into a pipe rather
 * than another spawned process's stdout. Reads to real EOF, then compares
 * byte-for-byte against the exact text kmain.c's hereDriverThread types
 * after `<<<` plus the single trailing newline jobctlSpawnHereStringPiped
 * always appends (matching a real shell's own `<<<` behavior of feeding
 * the text as one full line, terminator included).
 */
#define hereStringPayload "hs-ok\n"

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
    print("herestest: reading redirected stdin\n");

    char buffer[32];
    unsigned long total = 0;
    for (;;) {
        long got = userRead(0, buffer + total, sizeof(buffer) - 1 - total);
        if (got < 0) {
            print("herestest: FAIL read\n");
            return 1;
        }
        if (got == 0)
            break;
        total += (unsigned long)got;
        if (total >= sizeof(buffer) - 1)
            break;
    }
    buffer[total] = 0;

    print("herestest: got: ");
    print(buffer);

    int ok = streq(buffer, hereStringPayload);
    print(ok ? "herestest: payload-ok\n" : "herestest: payload-FAIL\n");

    return ok ? 0 : 1;
}
