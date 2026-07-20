#include "ulib.h"

/*
 * Boot-spawned independently of consolereader2.c (see kmain.c's spawnUser
 * calls) -- like namedreader.c/namedwriter.c, neither process's pid, argv,
 * or fd table ever mentions the other. Both simply read fd 0, which every
 * process gets for free (vfs.c's vfsFdInit) as its own console reader queue
 * (keyboard.h) rather than a single kernel-wide buffer: proving that two
 * completely unrelated processes reading "the keyboard" each get their own
 * independent copy of every keystroke instead of racing each other for it.
 *
 * kmain.c's consoleInjectorThread fires exactly one synthetic keystroke a
 * few hundred milliseconds after boot (there is no automated way to drive a
 * real PS/2 device under headless QEMU, see its own comment) through the
 * same keyboardHandleScancode the real IRQ handler calls -- so this process
 * has to genuinely park in userRead across many timer ticks and be woken
 * later, not just win a race against a byte that was already queued.
 */

int main(void)
{
    print("consolereader1: start, pid=");
    printDec(userGetpid());
    print(", blocking on fd 0\n");

    char c = 0;
    long got = userRead(0, &c, 1);
    if (got != 1) {
        print("consolereader1: FAIL read\n");
        return 1;
    }

    print("consolereader1: got byte '");
    userWrite(1, &c, 1);
    print("' -> console-fanout-1-ok\n");
    return 0;
}
