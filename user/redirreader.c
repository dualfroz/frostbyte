#include "ulib.h"

/*
 * Companion to redirwriter.c -- reads fd 0 (redirected by the shell to a
 * REAL file via `<`, jobctlApplyRedirects) in a loop until a real EOF (a
 * regular file's own vfsRead already returns 0 the instant the offset
 * reaches its size -- no writer-count bookkeeping needed the way
 * pipesink.c's pipe-backed EOF does, since a file's "end" is just its
 * length, not "every writer has gone away"), then compares byte-for-byte
 * against two back-to-back copies of redirwriter.c's own payload -- proof
 * that `>` (create/truncate) followed by `>>` (append) on the SAME path
 * produced exactly that file, not a truncated/duplicated/reordered one.
 *
 * Unlike pipesink.c, this process's own fd 1 (stdout) is NOT redirected --
 * print()/printDec() (ulib.h) reach the real console exactly like any
 * unredirected demo, so the needle below is exactly as observable to
 * ci-test.sh as every other one.
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
    print("redirreader: reading redirected stdin\n");

    char buffer[80];
    unsigned long total = 0;
    for (;;) {
        long got = userRead(0, buffer + total, sizeof(buffer) - 1 - total);
        if (got < 0) {
            print("redirreader: FAIL read\n");
            return 1;
        }
        if (got == 0)
            break;
        total += (unsigned long)got;
        if (total >= sizeof(buffer) - 1)
            break;
    }
    buffer[total] = 0;

    print("redirreader: got: ");
    print(buffer);
    print("\n");

    int ok = streq(buffer, "redirect-payload-okredirect-payload-ok");
    print(ok ? "redirreader: payload-ok\n" : "redirreader: payload-FAIL\n");

    return ok ? 0 : 1;
}
