#include "ulib.h"

static int streq(const char* a, const char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        print("fiforeader: FAIL expected one argv (pipe handle)\n");
        return 1;
    }

    long handle = parseDec(argv[1]);
    long fd = userPipeAttach(handle, 0);
    if (fd < 0) {
        print("fiforeader: FAIL attach\n");
        return 1;
    }

    print("fiforeader: attached, blocking on read\n");
    char buffer[64];
    long got = userRead((int)fd, buffer, sizeof(buffer) - 1);
    if (got <= 0) {
        print("fiforeader: FAIL read\n");
        return 1;
    }
    buffer[got] = 0;

    print("fiforeader: got: ");
    print(buffer);
    print("\n");

    int ok = streq(buffer, "shared pipe payload");
    print(ok ? "fiforeader: payload-ok\n" : "fiforeader: payload-FAIL\n");

    userClose((int)fd);
    return ok ? 0 : 1;
}
