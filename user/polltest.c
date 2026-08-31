#include "ulib.h"

/*
 * Boot-time driver for real poll(2) (poll.h's PollFd, syscall.c's sysPoll,
 * sched.c's schedPollChannel + schedule()'s own pollDeadline expiry scan):
 * waits for readiness across MORE THAN ONE fd at once, something no
 * earlier demo in this kernel does -- every previous blocking read/write
 * always committed ahead of time to exactly one fd (vfsBlockingChannel,
 * vfs.h). Three phases:
 *
 *   phase 1: a non-blocking poll (timeoutMs 0) on a shared pipe's read end
 *   nobody has written to yet -- must report zero ready fds WITHOUT ever
 *   parking the calling thread, proven simply by returning at all (a real
 *   accidental-park bug here would hang the whole boot to ci-test.sh's 25s
 *   QEMU timeout instead of merely failing a check, the same
 *   fault-injection-visible shape every other demo in this kernel already
 *   has).
 *
 *   phase 2: a REAL, bounded-timeout poll on that same still-empty fd --
 *   proves schedule()'s pollDeadline expiry scan actually forces a wake
 *   when no fd ever becomes ready, not just phase 1's immediate-return
 *   fast path.
 *
 *   phase 3: an INFINITE-timeout (-1) poll across TWO fds at once (the
 *   pipe pollwriter.c will write to, and a SECOND, entirely unrelated pipe
 *   nobody ever writes -- deliberately not fd 0/console: every open
 *   reader, including this process's own fd 0, gets a copy of every
 *   keystroke keyboardHandleScancode ever broadcasts, keyboard.h, and this
 *   same boot image has other demos genuinely typing at the shell
 *   concurrently, so fd 0 could not be trusted to stay silent for the
 *   whole wait) while a freshly spawned child (pollwriter.c) sleeps
 *   briefly then writes to the first pipe -- proves the real cross-process
 *   wake path (pipeWrite -> schedWakeChannel(schedPollChannel()) -> this
 *   thread's retried sysPoll re-checking every fd from scratch) AND that
 *   only the written pipe's own PollFd entry gets revents set, never the
 *   untouched second one's, i.e. the multiplexer is genuinely selective,
 *   not just "anything happened, assume the first entry".
 */

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

    /* Never written to by anyone -- the second phase-3 fd that must stay
       not-ready for the whole wait. A private, unshared userPipe() end
       (rather than a second userPipeCreate/userPipeAttach handle) is
       enough since nothing else ever touches it. */
    int idleFds[2];
    if (userPipe(idleFds) != 0) {
        print("polltest: FAIL idle pipe create\n");
        return 1;
    }

    /* Spawned immediately, like fifomaster.c spawns its own two children,
       rather than after phase 1/2 do their own work below -- every other
       boot-time demo's runtime spawn happens right at its own start, and
       pollwriter's own delay (pollWriterDelayMs, pollwriter.c) is already
       comfortably longer than phase 2's bounded-timeout wait below, so
       phase 3 still genuinely blocks waiting for it either way. */
    char handleStr[24];
    decToString(handle, handleStr);
    char* writerArgv[] = { "/bin/pollwriter", handleStr, 0 };
    long writerPid = userSpawn("/bin/pollwriter", writerArgv);
    if (writerPid < 0) {
        print("polltest: FAIL spawn pollwriter\n");
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
