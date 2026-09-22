#include "ulib.h"

static volatile int handlerUsr1Fired = 0;
static volatile int handlerUsr2Fired = 0;

static void onUsr1(int signum)
{
    (void)signum;
    handlerUsr1Fired = 1;
    print("restarttarget: usr1-handler-fired\n");
}

static void onUsr2(int signum)
{
    (void)signum;
    handlerUsr2Fired = 1;
    print("restarttarget: usr2-handler-fired\n");
}

int main(int argc, char** argv)
{
    print("restarttarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 5) {
        print("restarttarget: FAIL expected four argv (ready/announce1/announce2/data handles)\n");
        return 1;
    }

    if (userSigAction(sigUsr1, onUsr1, 0, 0) == (long)sigSetFailed ||
        userSigAction(sigUsr2, onUsr2, 0, sigFlagRestart) == (long)sigSetFailed) {
        print("restarttarget: FAIL install handlers\n");
        return 1;
    }

    long victimHandle = userPipeCreate();
    if (victimHandle < 0) {
        print("restarttarget: FAIL create victim pipe\n");
        return 1;
    }
    long victimFd = userPipeAttach(victimHandle, 0);
    if (victimFd < 0) {
        print("restarttarget: FAIL attach victim pipe\n");
        return 1;
    }

    long readyHandle = parseDec(argv[1]);
    long announce1Handle = parseDec(argv[2]);
    long announce2Handle = parseDec(argv[3]);
    long dataHandle = parseDec(argv[4]);

    long readyFd = userPipeAttach(readyHandle, 1);
    long announce1Fd = userPipeAttach(announce1Handle, 1);
    long announce2Fd = userPipeAttach(announce2Handle, 1);
    long dataFd = userPipeAttach(dataHandle, 0);
    if (readyFd < 0 || announce1Fd < 0 || announce2Fd < 0 || dataFd < 0) {
        print("restarttarget: FAIL attach ready/announce/data pipes\n");
        return 1;
    }

    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("restarttarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("restarttarget: ready, handlers installed\n");

    print("restarttarget: phase1 blocking on victim read (expect errInterrupted)\n");
    if (userWrite((int)announce1Fd, "a", 1) != 1) {
        print("restarttarget: FAIL write announce1 byte\n");
        return 1;
    }
    userClose((int)announce1Fd);

    char victimBuffer[4];
    long n1 = userRead((int)victimFd, victimBuffer, sizeof(victimBuffer));
    userClose((int)victimFd);

    if (!handlerUsr1Fired) {
        print("restarttarget: FAIL usr1 handler never fired\n");
        return 1;
    }
    if (n1 != errInterrupted) {
        print("restarttarget: FAIL victim read did not return errInterrupted\n");
        return 1;
    }
    print("restarttarget: phase1-eintr-no-restart-ok\n");

    print("restarttarget: phase2 blocking on data read (expect transparent restart)\n");
    if (userWrite((int)announce2Fd, "a", 1) != 1) {
        print("restarttarget: FAIL write announce2 byte\n");
        return 1;
    }
    userClose((int)announce2Fd);

    char dataByte = 0;
    long n2 = userRead((int)dataFd, &dataByte, 1);
    userClose((int)dataFd);

    if (!handlerUsr2Fired) {
        print("restarttarget: FAIL usr2 handler never fired\n");
        return 1;
    }
    if (n2 != 1 || dataByte != 'd') {
        print("restarttarget: FAIL data read did not transparently resume with real data\n");
        return 1;
    }
    print("restarttarget: phase2-restart-transparent-ok\n");

    print("restarttarget: all-phases-ok\n");
    return 55;
}
