#include "ulib.h"

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
