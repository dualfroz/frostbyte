#include "ulib.h"

#define namedPipePath "/tmp/rendezvous"

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
    print("namedreader: start, pid=");
    printDec(userGetpid());
    print("\n");

    long fd = userOpen(namedPipePath, oReadOnly);
    if (fd < 0) {
        print("namedreader: FAIL open\n");
        return 1;
    }

    print("namedreader: opened, blocking on read\n");
    char buffer[64];
    long got = userRead((int)fd, buffer, sizeof(buffer) - 1);
    if (got <= 0) {
        print("namedreader: FAIL read\n");
        return 1;
    }
    buffer[got] = 0;

    print("namedreader: got: ");
    print(buffer);
    print("\n");

    int ok = streq(buffer, "hello via named pipe");
    print(ok ? "namedreader: payload-ok\n" : "namedreader: payload-FAIL\n");

    userClose((int)fd);
    return ok ? 0 : 1;
}
