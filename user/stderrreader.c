#include "ulib.h"

/*
 * Companion to stderrwriter.c -- reads fd 0 (redirected by the shell to a
 * REAL file via `<`, jobctlApplyRedirects) in a loop until a real EOF,
 * exactly like redirreader.c does, then compares byte-for-byte against two
 * back-to-back copies of stderrwriter.c's own payload -- proof that `2>`
 * (create/truncate) followed by `2>>` (append) on the SAME path produced
 * exactly that file, not a truncated/duplicated/reordered one.
 *
 * This process's own fd 1 (stdout) is NOT redirected -- print() (ulib.h)
 * reaches the real console exactly like any unredirected demo, so the
 * needle below is exactly as observable to ci-test.sh as every other one.
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
    print("stderrreader: reading redirected stdin\n");

    char buffer[80];
    unsigned long total = 0;
    for (;;) {
        long got = userRead(0, buffer + total, sizeof(buffer) - 1 - total);
        if (got < 0) {
            print("stderrreader: FAIL read\n");
            return 1;
        }
        if (got == 0)
            break;
        total += (unsigned long)got;
        if (total >= sizeof(buffer) - 1)
            break;
    }
    buffer[total] = 0;

    print("stderrreader: got: ");
    print(buffer);
    print("\n");

    int ok = streq(buffer, "stderr-payload-okstderr-payload-ok");
    print(ok ? "stderrreader: payload-ok\n" : "stderrreader: payload-FAIL\n");

    return ok ? 0 : 1;
}
