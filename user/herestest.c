#include "ulib.h"

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
