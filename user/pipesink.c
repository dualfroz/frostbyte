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
