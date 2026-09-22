#include "ulib.h"

#define namedPipePath "/tmp/rendezvous"
#define namedWriterDelayMs 200

static const char* payload = "hello via named pipe";

int main(void)
{
    print("namedwriter: start, pid=");
    printDec(userGetpid());
    print("\n");

    userSleep(namedWriterDelayMs);

    long fd = userOpen(namedPipePath, oWriteOnly);
    if (fd < 0) {
        print("namedwriter: FAIL open\n");
        return 1;
    }

    long written = userWrite((int)fd, payload, stringLength(payload));
    if (written != (long)stringLength(payload)) {
        print("namedwriter: FAIL short/failed write\n");
        return 1;
    }
    print("namedwriter: wrote payload\n");

    userClose((int)fd);
    return 0;
}
