#include "ulib.h"

/*
 * Boot-time, single-process live demo of sigFlagNoDefer (signalnum.h /
 * signal.c's sigConsumePending): unlike every other signal demo in this
 * kernel (nesttest/masktest/jobtest/...), this needs no second process --
 * SA_NODEFER is entirely about whether a signal can interrupt ITS OWN
 * still-active handler, a property one process can raise against itself
 * and observe directly, with no inter-process rendezvous required at all.
 *
 * Two contrasting handlers, proven in the same run:
 *
 *   onUsr1 is registered WITH sigFlagNoDefer. Its first invocation
 *   self-raises sigUsr1 again (userSigSend(selfPid, sigUsr1)) while still
 *   active. Since sigFlagNoDefer means sigUsr1's own bit was never added
 *   to sigBlocked, that re-raise is genuinely deliverable the instant the
 *   self-raise syscall itself returns to ring 3 (isr.c's
 *   deliverPendingSignals, unconditional on every trap return) -- entering
 *   a SECOND, NESTED instance of onUsr1 before the first one's own
 *   userSigSend call ever returns to it. The nested instance sees
 *   usr1EnterCount already 2 and does not re-raise again (bounded
 *   recursion), returns via the trampoline's automatic sysSigReturn,
 *   letting the outer instance resume exactly where its self-raise call
 *   left off and observe the count already at 2 -- proof the nesting
 *   already happened, not a race it merely hopes for.
 *
 *   onUsr2 is registered WITHOUT the flag (plain default, like every other
 *   handler in this kernel before this session). Its first invocation
 *   self-raises sigUsr2 the same way, but sigUsr2's own bit IS in
 *   sigBlocked for as long as onUsr2 is active, so that re-raise's own
 *   syscall returns without anything nesting -- onUsr2 finishes its first
 *   invocation having observed no nested re-entry at all (the negative
 *   half of the proof), and only once it actually returns (sysSigReturn
 *   restoring sigBlocked without sigUsr2's self-block, isrDispatch
 *   immediately re-checking pending signals on that very trap return,
 *   exactly like masktarget.c's own deferred-delivery proof) does the
 *   deferred second raise finally arrive, as its own fresh TOP-LEVEL
 *   invocation, never nested.
 *
 * No sleep/timing guess anywhere: every self-raise's pending-bit write
 * happens synchronously inside the syscall against this exact same
 * thread, and delivery/deferral is entirely decided by sigBlocked at the
 * next trap return, which happens deterministically at each userSigSend's
 * or handler-return's own int 0x80 -- there is no window for either
 * outcome to be a coincidence of scheduling.
 */

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
        /* If sigFlagNoDefer genuinely lifted the self-block, the nested
           instance of THIS SAME function already ran to completion, in
           full, during the userSigSend call above (its own return-to-ring3
           delivered it before ever coming back here) -- so usr1EnterCount
           must already read 2 by the time control resumes at this exact
           line, not merely "eventually". */
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
        /* Unlike onUsr1 above: sigUsr2's own bit IS self-blocked here (no
           sigFlagNoDefer), so the re-raise above must NOT have nested --
           usr2EnterCount must still read exactly 1 at this exact line. */
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

    /* This syscall's own return-to-ring3 is what delivers onUsr1's FIRST
       invocation -- the nested second one (see onUsr1's own comment)
       happens entirely inside that delivery, synchronously, before this
       call ever returns to main() below. */
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

    /* Same shape for onUsr2: this call's return delivers its first
       invocation; the DEFERRED second one (no nesting) is delivered on
       that first invocation's own sysSigReturn, still inside this single
       userSigSend's worth of ring-3 return handling, before control ever
       reaches back here. */
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
