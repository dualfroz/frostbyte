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
