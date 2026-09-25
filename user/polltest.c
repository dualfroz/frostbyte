#include "ulib.h"

#define pollTimeoutMs 200

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
    print("polltest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long handle = userPipeCreate();
    if (handle < 0) {
        print("polltest: FAIL pipe create\n");
        return 1;
    }
    long readFd = userPipeAttach(handle, 0);
    if (readFd < 0) {
        print("polltest: FAIL attach read end\n");
        return 1;
    }

    int idleFds[2];
    if (userPipe(idleFds) != 0) {
        print("polltest: FAIL idle pipe create\n");
        return 1;
    }

    PollFd single;
    single.fd = (int)readFd;
    single.events = pollIn;
    single.revents = 12345;
    long n = userPoll(&single, 1, 0);
    if (n != 0 || single.revents != 0) {
        print("polltest: FAIL non-blocking poll on empty pipe\n");
        return 1;
    }
    print("polltest: non-blocking-poll-empty-ok\n");

    single.revents = 12345;
    n = userPoll(&single, 1, pollTimeoutMs);
    if (n != 0 || single.revents != 0) {
        print("polltest: FAIL bounded-timeout poll returned early or ready\n");
        return 1;
    }
    print("polltest: poll-timeout-ok\n");

    char handleStr[24];
    decToString(handle, handleStr);
    char* writerArgv[] = { "/bin/pollwriter", handleStr, 0 };
    long writerPid = userSpawn("/bin/pollwriter", writerArgv);
    if (writerPid < 0) {
        print("polltest: FAIL spawn pollwriter\n");
        return 1;
    }

    PollFd pfds[2];
    pfds[0].fd = (int)readFd;
    pfds[0].events = pollIn;
    pfds[0].revents = 0;
    pfds[1].fd = idleFds[0];
    pfds[1].events = pollIn;
    pfds[1].revents = 0;

    print("polltest: blocking on multi-fd poll, waiting for pollwriter\n");
    n = userPoll(pfds, 2, -1);
    if (n != 1 || !(pfds[0].revents & pollIn) || pfds[1].revents != 0) {
        print("polltest: FAIL multi-fd poll woke wrong or with wrong revents\n");
        return 1;
    }
    print("polltest: poll-woken-by-write-ok\n");

    char buffer[32];
    long got = userRead((int)readFd, buffer, sizeof(buffer) - 1);
    if (got <= 0) {
        print("polltest: FAIL read after poll-ready\n");
        return 1;
    }
    buffer[got] = 0;

    int ok = streq(buffer, "poll payload");
    print(ok ? "polltest: payload-ok\n" : "polltest: payload-FAIL\n");
    if (!ok)
        return 1;

    long writerStatus = userWait(writerPid);
    if (writerStatus != 0) {
        print("polltest: FAIL pollwriter reported failure\n");
        return 1;
    }

    userClose((int)readFd);
    userClose(idleFds[0]);
    userClose(idleFds[1]);
    print("polltest: poll-multiplex-ok\n");
    return 0;
}
