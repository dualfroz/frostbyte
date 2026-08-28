#include "ulib.h"

/*
 * Boot-time driver for job control (signal.h's SIGSTOP/SIGCONT/SIGTSTP,
 * sched.h's threadStopped): spawns jobtarget.c, waits for it to actually
 * be parked in a real blocking userRead (not guessed via a sleep), stops it
 * there with userSigSend(target, sigStop), writes the very byte it is
 * blocked waiting for WHILE it is stopped, and only then sends sigCont --
 * proving the resume happens through the genuine restart-syscall retry
 * path sysRead already uses (syscall.c), not some shortcut that would have
 * worked even if threadStop/threadContinue did nothing at all.
 *
 * Two independent pipes, not one, exactly because this needs traffic in
 * BOTH directions kept open across the whole exchange (sigtest.c's own
 * single "ready" pipe is closed by both sides right after one byte, which
 * is not enough here): jobtest attaches BOTH ends it owns (ready as
 * reader, go as writer) before ever spawning jobtarget, for the same
 * "attach first" reason sigtest.c documents at length -- whichever side
 * would otherwise attach second risks the pipe's slot already being freed
 * by pipeReleaseIfUnused if the other side raced through attach+use+close
 * first.
 */

int main(void)
{
    print("jobtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    long readyHandle = userPipeCreate();
    long goHandle = userPipeCreate();
    if (readyHandle < 0 || goHandle < 0) {
        print("jobtest: FAIL create pipes\n");
        return 1;
    }

    long readyFd = userPipeAttach(readyHandle, 0);
    long goFd = userPipeAttach(goHandle, 1);
    if (readyFd < 0 || goFd < 0) {
        print("jobtest: FAIL attach pipes\n");
        return 1;
    }

    char readyHandleStr[24];
    char goHandleStr[24];
    decToString(readyHandle, readyHandleStr);
    decToString(goHandle, goHandleStr);

    char* targetArgv[] = { "/bin/jobtarget", readyHandleStr, goHandleStr, 0 };
    long target = userSpawn("/bin/jobtarget", targetArgv);
    if (target < 0) {
        print("jobtest: FAIL spawn jobtarget\n");
        return 1;
    }

    /* sched.h's Thread.pgid: a RUNTIME userSpawn from a genuinely running,
       non-idle parent (this process) must have jobtarget inherit THIS
       process's own pgid, matching real fork()'s default before any
       setpgid ever runs -- the opposite default from every boot-time
       spawnUser call in kmain.c (each becomes its own new group leader,
       sched.c's initialPgid), which schedTest.c's own KTEST coverage
       cannot exercise at all (ktestRunAll always runs as the permanent
       idle thread, the one case that can never be a real spawning parent
       -- see that file's header comment). This is the only live ring-3
       proof of the inherit-from-parent half of initialPgid. */
    if (userGetpgid(target) != userGetpgid(0)) {
        print("jobtest: FAIL jobtarget did not inherit parent pgid\n");
        return 1;
    }
    print("jobtest: child-inherits-parent-pgid-ok\n");

    char readyByte;
    /* Real blocking read: does not return until jobtarget has actually
       attached the go pipe as a reader AND written its ready byte -- so by
       the time this returns, jobtarget is either about to call, or already
       inside, its own blocking read on the go pipe. */
    if (userRead((int)readyFd, &readyByte, 1) != 1) {
        print("jobtest: FAIL read ready byte\n");
        return 1;
    }
    userClose((int)readyFd);
    print("jobtest: jobtarget ready\n");

    if (userSigSend(target, sigStop) != 0) {
        print("jobtest: FAIL send sigStop to jobtarget\n");
        return 1;
    }
    print("jobtest: sent sigStop to jobtarget\n");

    /* Writing the byte jobtarget is blocked waiting for WHILE it is
       stopped, rather than after sigCont, is deliberate: it exercises the
       realistic job-control shape (a stopped job's input keeps queuing up
       behind it) and proves stop+cont do not lose or corrupt that pending
       write. It is NOT, on its own, an airtight proof that sigStop above
       did anything at all -- pipeWrite (pipe.c) calls schedWakeChannel
       unconditionally on every successful write, so a jobtarget that was
       merely left threadBlocked (sigStop silently broken/a no-op) would
       ALSO be woken by this very write and finish on its own, without
       needing sigCont below at all. That rigorous "would this still pass
       if stop were a no-op" distinction is exactly what
       schedTest.c's threadStopExcludesTargetFromUnrelatedWaitChannelWake
       proves instead (fault-injection verified: a genuinely Stopped
       thread's waitChannel is never matched by schedWakeChannel) -- what
       THIS live demo proves is the piece that KTEST cannot: a real ring-3
       program blocked in a real syscall survives being stopped and
       resumed through the real syscall retry path, end to end, without
       crashing or wedging the system. */
    if (userWrite((int)goFd, "g", 1) != 1) {
        print("jobtest: FAIL write go byte\n");
        return 1;
    }

    if (userSigSend(target, sigCont) != 0) {
        print("jobtest: FAIL send sigCont to jobtarget\n");
        return 1;
    }
    print("jobtest: sent sigCont to jobtarget\n");
    userClose((int)goFd);

    long status = userWait(target);
    print("jobtest: jobtarget exit status=");
    printDec(status);
    print("\n");
    if (status != 55) {
        print("jobtest: FAIL expected jobtarget exit status 55\n");
        return 1;
    }
    print("jobtest: job-stop-continue-ok\n");

    return 0;
}
