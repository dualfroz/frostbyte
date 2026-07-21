#include "ulib.h"

/*
 * Spawned by mmaptest.c to prove that a real jump into a mapping that never
 * asked for mmapProtExec turns into a genuine SIGSEGV (isr.c's tryResolve-
 * UserPageFault I/D-bit short-circuit, not an infinite fault loop and not
 * a kernel-wide panic) rather than silently letting the CPU execute
 * whatever bytes happen to sit there -- the exact NX enforcement gap
 * fs.h's own mmapProtExec doc comment used to name as unimplemented. This
 * process is expected to die, not return 0; its own final "FAIL" print/
 * return only runs if that protection was somehow NOT enforced.
 */

typedef void (*victimFn)(void);

int main(void)
{
    print("mmapexecvictim: start\n");

    void* addr = userMmap(0, 4096, mmapProtRead | mmapProtWrite, mmapAnonymous, -1, 0);
    if (addr == userMmapFailed) {
        print("mmapexecvictim: FAIL mmap\n");
        return 1;
    }

    /* A real `ret` instruction (0xC3) -- harmless if it ever actually runs,
       since the point is that it must never get the chance to. */
    volatile unsigned char* code = (volatile unsigned char*)addr;
    code[0] = 0xC3;
    print("mmapexecvictim: write-ok\n");

    /* PROT_READ|PROT_WRITE only -- no PROT_EXEC -- so this call's own
       instruction fetch must be refused by the CPU's own NX bit and turn
       into a real SIGSEGV delivered against THIS process alone. */
    victimFn fn = (victimFn)addr;
    fn();

    print("mmapexecvictim: FAIL exec should have segfaulted\n");
    return 1;
}
