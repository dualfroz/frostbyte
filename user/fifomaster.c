#include "ulib.h"

int main(void)
{
    print("fifomaster: start, pid=");
    printDec(userGetpid());
    print("\n");

    long handle = userPipeCreate();
    if (handle < 0) {
        print("fifomaster: FAIL pipe create\n");
        return 1;
    }

    char handleStr[24];
    decToString(handle, handleStr);

    char* readerArgv[] = { "/bin/fiforeader", handleStr, 0 };
    long readerPid = userSpawn("/bin/fiforeader", readerArgv);
    if (readerPid < 0) {
        print("fifomaster: FAIL spawn fiforeader\n");
        return 1;
    }

    char* writerArgv[] = { "/bin/fifowriter", handleStr, 0 };
    long writerPid = userSpawn("/bin/fifowriter", writerArgv);
    if (writerPid < 0) {
        print("fifomaster: FAIL spawn fifowriter\n");
        return 1;
    }

    long readerStatus = userWait(readerPid);
    long writerStatus = userWait(writerPid);

    if (readerStatus != 0 || writerStatus != 0) {
        print("fifomaster: FAIL reader or writer reported failure\n");
        return 1;
    }

    print("fifomaster: pipeshare ok\n");
    return 0;
}
