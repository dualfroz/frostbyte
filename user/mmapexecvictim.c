#include "ulib.h"

typedef void (*victimFn)(void);

int main(void)
{
    print("mmapexecvictim: start\n");

    void* addr = userMmap(0, 4096, mmapProtRead | mmapProtWrite, mmapAnonymous, -1, 0);
    if (addr == userMmapFailed) {
        print("mmapexecvictim: FAIL mmap\n");
        return 1;
    }

    volatile unsigned char* code = (volatile unsigned char*)addr;
    code[0] = 0xC3;
    print("mmapexecvictim: write-ok\n");

    victimFn fn = (victimFn)addr;
    fn();

    print("mmapexecvictim: FAIL exec should have segfaulted\n");
    return 1;
}
