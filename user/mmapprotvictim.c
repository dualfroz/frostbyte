#include "ulib.h"

/*
 * Spawned by mmaptest.c to prove that a real write against a PROT_READ-only
 * mapping turns into a genuine SIGSEGV (isr.c's #PF handler) rather than a
 * kernel-wide panic -- this process is expected to die, not return 0. Its
 * own final "FAIL" print/return only runs if that protection was somehow
 * NOT enforced.
 */

int main(void)
{
    print("mmapprotvictim: start\n");

    int fd = userOpen("/etc/motd.txt", oReadOnly);
    if (fd < 0) {
        print("mmapprotvictim: FAIL open\n");
        return 1;
    }

    void* addr = userMmap(0, 4096, mmapProtRead, 0, fd, 0);
    if (addr == userMmapFailed) {
        print("mmapprotvictim: FAIL mmap\n");
        return 1;
    }

    volatile char* p = (volatile char*)addr;
    if (p[0] != 'f') {
        print("mmapprotvictim: FAIL unexpected read content\n");
        return 1;
    }
    print("mmapprotvictim: read-ok\n");

    /* PROT_READ only -- this write must be refused by mmapFaultHandler and
       turn into a real SIGSEGV delivered against THIS process alone. */
    p[0] = 'X';

    print("mmapprotvictim: FAIL write should have segfaulted\n");
    return 1;
}
