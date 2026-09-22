#include "ulib.h"

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

    p[0] = 'X';

    print("mmapprotvictim: FAIL write should have segfaulted\n");
    return 1;
}
