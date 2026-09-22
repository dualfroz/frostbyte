#include "ulib.h"

#define fifoWriterDelayMs 200

static const char* payload = "shared pipe payload";

int main(int argc, char** argv)
{
    if (argc != 2) {
        print("fifowriter: FAIL expected one argv (pipe handle)\n");
        return 1;
    }

    long handle = parseDec(argv[1]);
    long fd = userPipeAttach(handle, 1);
    if (fd < 0) {
        print("fifowriter: FAIL attach\n");
        return 1;
    }

    print("fifowriter: attached, sleeping before write\n");
    userSleep(fifoWriterDelayMs);

    long written = userWrite((int)fd, payload, stringLength(payload));
    if (written != (long)stringLength(payload)) {
        print("fifowriter: FAIL short/failed write\n");
        return 1;
    }
    print("fifowriter: wrote payload\n");

    userClose((int)fd);
    return 0;
}
