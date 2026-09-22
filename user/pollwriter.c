#include "ulib.h"

#define pollWriterDelayMs 500

static const char* payload = "poll payload";

int main(int argc, char** argv)
{
    if (argc != 2) {
        print("pollwriter: FAIL expected one argv (pipe handle)\n");
        return 1;
    }

    long handle = parseDec(argv[1]);
    long fd = userPipeAttach(handle, 1);
    if (fd < 0) {
        print("pollwriter: FAIL attach\n");
        return 1;
    }

    print("pollwriter: attached, sleeping before write\n");
    userSleep(pollWriterDelayMs);

    long written = userWrite((int)fd, payload, stringLength(payload));
    if (written != (long)stringLength(payload)) {
        print("pollwriter: FAIL short/failed write\n");
        return 1;
    }
    print("pollwriter: wrote payload\n");

    userClose((int)fd);
    return 0;
}
