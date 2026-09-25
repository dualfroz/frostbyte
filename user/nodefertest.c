#include "ulib.h"

static long selfPid = -1;

static volatile int usr1EnterCount = 0;
static volatile int usr1NestedConfirmed = 0;

static void onUsr1(int signum)
{
    (void)signum;
    usr1EnterCount++;

    if (usr1EnterCount == 1) {
        print("nodefertest: onUsr1 first entry, self-raising sigUsr1 (NODEFER)\n");
        if (userSigSend(selfPid, sigUsr1) != 0) {
            print("nodefertest: FAIL self-raise sigUsr1\n");
            return;
        }
        if (usr1EnterCount >= 2) {
            usr1NestedConfirmed = 1;
            print("nodefertest: onUsr1 observed its own nested re-entry (nodefer-self-nest-ok)\n");
        } else {
            print("nodefertest: FAIL onUsr1 did not nest into itself\n");
        }
    } else {
        print("nodefertest: onUsr1 nested entry (depth 2)\n");
    }
}

static volatile int usr2EnterCount = 0;
static volatile int usr2WronglyNested = 0;

static void onUsr2(int signum)
{
    (void)signum;
    usr2EnterCount++;

    if (usr2EnterCount == 1) {
        print("nodefertest: onUsr2 first entry, self-raising sigUsr2 (default defer)\n");
        if (userSigSend(selfPid, sigUsr2) != 0) {
            print("nodefertest: FAIL self-raise sigUsr2\n");
            return;
        }
        if (usr2EnterCount != 1) {
            usr2WronglyNested = 1;
            print("nodefertest: FAIL onUsr2 nested into itself without sigFlagNoDefer\n");
        } else {
            print("nodefertest: onUsr2 confirmed no self-nest while deferred (still-deferred-ok)\n");
        }
    } else {
        print("nodefertest: onUsr2 second, TOP-LEVEL entry after first returned\n");
    }
}

int main(void)
{
    print("nodefertest: start, pid=");
    printDec(userGetpid());
    print("\n");
    selfPid = userGetpid();

    if (userSigAction(sigUsr1, onUsr1, 0, sigFlagNoDefer) == (long)sigSetFailed ||
        userSigAction(sigUsr2, onUsr2, 0, 0) == (long)sigSetFailed) {
        print("nodefertest: FAIL install handlers\n");
        return 1;
    }

    if (userSigSend(selfPid, sigUsr1) != 0) {
        print("nodefertest: FAIL send sigUsr1 to self\n");
        return 1;
    }

    if (!usr1NestedConfirmed) {
        print("nodefertest: FAIL usr1 nodefer self-nest never confirmed\n");
        return 1;
    }
    if (usr1EnterCount != 2) {
        print("nodefertest: FAIL expected exactly 2 usr1 entries, got ");
        printDec(usr1EnterCount);
        print("\n");
        return 1;
    }
    print("nodefertest: nodefer-self-nest-verified-ok\n");

    if (userSigSend(selfPid, sigUsr2) != 0) {
        print("nodefertest: FAIL send sigUsr2 to self\n");
        return 1;
    }

    if (usr2WronglyNested) {
        print("nodefertest: FAIL usr2 self-nested despite no sigFlagNoDefer\n");
        return 1;
    }
    if (usr2EnterCount != 2) {
        print("nodefertest: FAIL expected exactly 2 usr2 entries (deferred, not nested), got ");
        printDec(usr2EnterCount);
        print("\n");
        return 1;
    }
    print("nodefertest: default-defer-no-self-nest-verified-ok\n");

    print("nodefertest: sa-nodefer-contrast-ok\n");
    return 0;
}
