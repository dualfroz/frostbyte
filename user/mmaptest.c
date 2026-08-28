#include "ulib.h"

/*
 * Live ring-3 proof for mmap.c's real demand-paged mmap(2)/munmap(2),
 * through the actual sysMmap/sysMunmap syscall path (not just mmapTest.c's
 * own in-kernel KTEST coverage, which calls mmap.c's functions directly):
 * anonymous demand-zero fill, a real write landing on that same page, a
 * real file-backed mapping whose content matches the real file it names,
 * (via mmapprotvictim.c) a real SIGSEGV -- not a kernel panic -- when a
 * different process writes to a PROT_READ-only mapping, a real function
 * call succeeding against a mapping that WAS granted PROT_EXEC, and (via
 * mmapexecvictim.c) a real SIGSEGV when a different process jumps into a
 * mapping that was NOT granted it -- the hardware NX enforcement fs.h's
 * own mmapProtExec doc comment used to name as unimplemented.
 */

#define mmapTestPageSize 4096

static int bytesEqual(const volatile unsigned char* a, const char* b, unsigned long n)
{
    for (unsigned long i = 0; i < n; i++)
        if (a[i] != (unsigned char)b[i])
            return 0;
    return 1;
}

int main(void)
{
    print("mmaptest: start\n");

    void* anon = userMmap(0, 2 * mmapTestPageSize, mmapProtRead | mmapProtWrite, mmapAnonymous, -1, 0);
    if (anon == userMmapFailed) {
        print("mmaptest: FAIL anon mmap\n");
        return 1;
    }
    volatile unsigned char* ap = (volatile unsigned char*)anon;
    if (ap[0] != 0 || ap[mmapTestPageSize] != 0) {
        print("mmaptest: FAIL anon page not zero-filled\n");
        return 1;
    }
    print("mmaptest: anon-zero-fill-ok\n");

    ap[0] = 7;
    ap[mmapTestPageSize] = 9;
    if (ap[0] != 7 || ap[mmapTestPageSize] != 9) {
        print("mmaptest: FAIL anon write/read mismatch\n");
        return 1;
    }
    print("mmaptest: anon-write-read-ok\n");

    if (userMunmap(anon, 2 * mmapTestPageSize) != 0) {
        print("mmaptest: FAIL munmap anon\n");
        return 1;
    }
    print("mmaptest: anon-munmap-ok\n");

    int fd = userOpen("/etc/motd.txt", oReadOnly);
    if (fd < 0) {
        print("mmaptest: FAIL open motd\n");
        return 1;
    }

    void* filed = userMmap(0, mmapTestPageSize, mmapProtRead, 0, fd, 0);
    if (filed == userMmapFailed) {
        print("mmaptest: FAIL file mmap\n");
        return 1;
    }
    if (!bytesEqual((const volatile unsigned char*)filed, "frostbyte ramdisk file\n", 23)) {
        print("mmaptest: FAIL file-backed content mismatch\n");
        return 1;
    }
    print("mmaptest: file-map-content-ok\n");
    userClose(fd);

    long victim = userSpawn("/bin/mmapprotvictim", 0);
    if (victim < 0) {
        print("mmaptest: FAIL spawn mmapprotvictim\n");
        return 1;
    }
    long status = userWait(victim);
    print("mmaptest: victim exit status=");
    printDec(status);
    print("\n");
    if (status != 128 + sigSegv) {
        print("mmaptest: FAIL expected sigsegv exit status\n");
        return 1;
    }
    print("mmaptest: protection-sigsegv-ok\n");

    /* A mapping that WAS granted mmapProtExec really is executable, not
       just "not actively refused" -- a real function call through it,
       not merely a read/write, is the only proof that means anything
       given mmapexecvictim.c below proves the opposite direction. */
    void* execMem = userMmap(0, mmapTestPageSize, mmapProtRead | mmapProtWrite | mmapProtExec,
                              mmapAnonymous, -1, 0);
    if (execMem == userMmapFailed) {
        print("mmaptest: FAIL exec mmap\n");
        return 1;
    }
    /* x86_64: mov eax, 42; ret -- a real, self-contained function body. */
    unsigned char* code = (unsigned char*)execMem;
    code[0] = 0xB8;
    code[1] = 42;
    code[2] = 0;
    code[3] = 0;
    code[4] = 0;
    code[5] = 0xC3;
    int (*fn)(void) = (int (*)(void))execMem;
    int result = fn();
    if (result != 42) {
        print("mmaptest: FAIL exec-allowed call returned wrong value\n");
        return 1;
    }
    print("mmaptest: exec-allowed-ok\n");
    userMunmap(execMem, mmapTestPageSize);

    long execVictim = userSpawn("/bin/mmapexecvictim", 0);
    if (execVictim < 0) {
        print("mmaptest: FAIL spawn mmapexecvictim\n");
        return 1;
    }
    long execStatus = userWait(execVictim);
    print("mmaptest: exec victim exit status=");
    printDec(execStatus);
    print("\n");
    if (execStatus != 128 + sigSegv) {
        print("mmaptest: FAIL expected sigsegv exit status for exec victim\n");
        return 1;
    }
    print("mmaptest: exec-denied-sigsegv-ok\n");

    print("mmaptest: PASS\n");
    return 0;
}
