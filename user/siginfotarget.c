#include "ulib.h"

/*
 * Only ever spawned by siginfotest.c, with argv[1]=the ready-pipe handle
 * (decToString'd, exactly like sigtarget.c's own argv[1]) and argv[2]=
 * siginfotest's own pid (also decToString'd) -- the one extra argument
 * this process needs that sigtarget.c's own version of this rendezvous
 * did not, since proving SA_SIGINFO's si_pid is real means comparing it
 * against a KNOWN sender pid, not just observing "some nonzero value came
 * through".
 *
 * Otherwise the exact same happens-before shape sigtarget.c already
 * established (userSigActionInfo BEFORE the ready byte is written, a real
 * blocking read on siginfotest.c's side of the pipe before it ever calls
 * userSigSend) -- see that file's own comment for why a plain sleep is not
 * enough here either: sigUsr1's default disposition is Terminate, so a
 * signal delivered before the handler is installed would kill this
 * process outright.
 */

static volatile int handlerFired = 0;
static volatile int capturedSignum = 0;
static volatile int capturedCode = -1;
static volatile int capturedPid = -1;

static void onUsr1(int signum, SigInfo* info, void* context)
{
    (void)context;
    capturedSignum = signum;
    capturedCode = info->siCode;
    capturedPid = info->siPid;
    handlerFired = 1;
    print("siginfotarget: handler invoked signum=");
    printDec(signum);
    print(" siCode=");
    printDec(info->siCode);
    print(" siPid=");
    printDec(info->siPid);
    print("\n");
}

int main(int argc, char** argv)
{
    print("siginfotarget: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (argc != 3) {
        print("siginfotarget: FAIL expected two argv (pipe handle, parent pid)\n");
        return 1;
    }

    /* sigFlagSigInfo (signalnum.h) is what actually makes the kernel call
       onUsr1 with three arguments instead of one -- without it, this
       handler would be invoked as if it were a plain SigHandler, reading
       `info`/`context` out of whatever garbage rsi/rdx happened to hold
       (see userSigActionInfo's own doc comment, ulib.h). */
    if (userSigActionInfo(sigUsr1, onUsr1, 0, sigFlagSigInfo) == (long)sigSetFailed) {
        print("siginfotarget: FAIL install siginfo handler\n");
        return 1;
    }

    long parentPid = parseDec(argv[2]);

    long handle = parseDec(argv[1]);
    long readyFd = userPipeAttach(handle, 1);
    if (readyFd < 0) {
        print("siginfotarget: FAIL attach ready pipe\n");
        return 1;
    }
    if (userWrite((int)readyFd, "r", 1) != 1) {
        print("siginfotarget: FAIL write ready byte\n");
        return 1;
    }
    userClose((int)readyFd);

    print("siginfotarget: waiting for signal\n");

    for (int i = 0; i < 40 && !handlerFired; i++)
        userSleep(20);

    if (!handlerFired) {
        print("siginfotarget: FAIL handler never fired\n");
        return 1;
    }
    if (capturedSignum != sigUsr1) {
        print("siginfotarget: FAIL wrong signum delivered\n");
        return 1;
    }
    if (capturedCode != siCodeUser) {
        print("siginfotarget: FAIL expected siCodeUser\n");
        return 1;
    }
    if (capturedPid != (int)parentPid) {
        print("siginfotarget: FAIL si_pid=");
        printDec(capturedPid);
        print(" does not match real sender pid=");
        printDec(parentPid);
        print("\n");
        return 1;
    }
    print("siginfotarget: siginfo-sender-pid-ok\n");

    /* Reached only if sysSigReturn correctly restored the exact pre-signal
       context (same proof sigtarget.c's own tail comment gives), plus this
       time only if the trampoline's three-argument call into onUsr1 above
       (rdi/rsi/rdx all genuinely set up, not just rdi) did not corrupt
       anything either. */
    print("siginfotarget: resumed-after-siginfo-handler-ok\n");
    return 77;
}
