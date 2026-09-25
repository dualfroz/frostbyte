#include "ulib.h"

static void printLong(const char* label, long value)
{
    print(label);
    printDec(value);
    print("\n");
}

int main(void)
{
    print("pipetest: start, pid=");
    printDec(userGetpid());
    print("\n");

    int fds[2];
    long rc = userPipe(fds);
    printLong("pipetest: userPipe rc=", rc);
    if (rc < 0) {
        print("pipetest: pipe() failed\n");
        return 1;
    }
    int readFd = fds[0];
    int writeFd = fds[1];

    char probe[4];
    long earlyRead = userRead(readFd, probe, sizeof(probe));
    printLong("pipetest: read before write rc=", earlyRead);
    if (earlyRead != -1) {
        print("pipetest: FAIL expected -1 on empty open pipe\n");
        return 1;
    }

    const char* payload = "hello through the pipe\n";
    long written = userWrite(writeFd, payload, stringLength(payload));
    printLong("pipetest: wrote bytes=", written);
    if (written != (long)stringLength(payload)) {
        print("pipetest: FAIL short write\n");
        return 1;
    }

    char buffer[64];
    long got = userRead(readFd, buffer, sizeof(buffer) - 1);
    if (got < 0) {
        print("pipetest: FAIL read after write blocked\n");
        return 1;
    }
    buffer[got] = 0;
    print("pipetest: read back: ");
    print(buffer);

    userClose(writeFd);

    long afterClose = userRead(readFd, probe, sizeof(probe));
    printLong("pipetest: read after writer closed rc=", afterClose);
    if (afterClose != 0) {
        print("pipetest: FAIL expected EOF (0) after writer closed\n");
        return 1;
    }

    userClose(readFd);

    int fds2[2];
    userPipe(fds2);
    userClose(fds2[0]);
    long brokenWrite = userWrite(fds2[1], "x", 1);
    printLong("pipetest: write after reader closed rc=", brokenWrite);
    if (brokenWrite != -1) {
        print("pipetest: FAIL expected broken pipe (-1)\n");
        return 1;
    }
    userClose(fds2[1]);

    print("pipetest: done\n");
    return 0;
}
