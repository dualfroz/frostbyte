#include "ulib.h"

/*
 * Drives the shared-pipe demo added this wave (pipeCreateDetached/
 * pipeAddReader/pipeAddWriter, pipe.h; vfsPipeCreateHandle/vfsPipeAttach,
 * vfs.h; sysRead/sysWrite's retry-the-syscall path, syscall.c): creates one
 * detached pipe handle and hands it to two INDEPENDENTLY spawned processes
 * as a decimal argv string (decToString/parseDec, ulib.c) rather than a
 * shared fd -- frostbyte has no fork(), so a raw pipe.h handle number is
 * the only thing two unrelated processes can rendezvous on. fifowriter.c
 * deliberately sleeps before writing, so fiforeader.c's read() genuinely
 * has to block across several timer ticks and be woken later, not just win
 * a scheduling race against an already-full buffer -- see fifowriter.c/
 * fiforeader.c for how each end proves that on its own side.
 */

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
