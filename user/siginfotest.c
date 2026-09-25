#include "ulib.h"

static volatile int chldFired = 0;
static volatile int chldCode = -1;
static volatile int chldPid = -1;
static volatile int chldStatus = -1;

static void onChld(int signum, SigInfo* info, void* context)
{
    (void)signum;
    (void)context;
    chldCode = info->siCode;
    chldPid = info->siPid;
    chldStatus = info->siStatus;
    chldFired = 1;
    print("siginfotest: sigchld-info-handler-fired siCode=");
    printDec(info->siCode);
    print(" siPid=");
    printDec(info->siPid);
    print(" siStatus=");
    printDec(info->siStatus);
    print("\n");
}

int main(void)
{
    print("siginfotest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userSigActionInfo(sigChld, onChld, 0, sigFlagSigInfo) == (long)sigSetFailed) {
        print("siginfotest: FAIL install siginfo sigchld handler\n");
        return 1;
    }

    long readyHandle = userPipeCreate();
    if (readyHandle < 0) {
        print("siginfotest: FAIL create ready pipe\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 0);
    if (readyFd < 0) {
        print("siginfotest: FAIL attach ready pipe\n");
        return 1;
    }

    char readyHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    char ownPidStr[24];
    decToString(userGetpid(), ownPidStr);

    char* targetArgv[] = { "/bin/siginfotarget", readyHandleStr, ownPidStr, 0 };
    long target = userSpawn("/bin/siginfotarget", targetArgv);
    if (target < 0) {
        print("siginfotest: FAIL spawn siginfotarget\n");
        return 1;
    }

    char readyByte;
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("siginfotest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    if (userSigSend(target, sigUsr1) != 0) {
        print("siginfotest: FAIL send sigUsr1 to siginfotarget\n");
        return 1;
    }
    print("siginfotest: sent sigUsr1 to siginfotarget\n");

    for (int i = 0; i < 80 && !chldFired; i++)
        userSleep(20);

    if (!chldFired) {
        print("siginfotest: FAIL sigchld siginfo handler never fired\n");
        return 1;
    }
    if (chldCode != siCodeChldExited) {
        print("siginfotest: FAIL expected siCodeChldExited\n");
        return 1;
    }
    if (chldPid != (int)target) {
        print("siginfotest: FAIL si_pid=");
        printDec(chldPid);
        print(" does not match real child pid=");
        printDec(target);
        print("\n");
        return 1;
    }
    if (chldStatus != 77) {
        print("siginfotest: FAIL si_status=");
        printDec(chldStatus);
        print(" does not match real child exit code 77\n");
        return 1;
    }
    print("siginfotest: sigchld-siginfo-accurate-ok\n");

    long status = userWait(target);
    print("siginfotest: siginfotarget exit status=");
    printDec(status);
    print("\n");
    if (status != 77) {
        print("siginfotest: FAIL expected siginfotarget exit status 77\n");
        return 1;
    }
    print("siginfotest: siginfo-roundtrip-ok\n");

    return 0;
}
