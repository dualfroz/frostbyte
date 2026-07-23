#include "ktest.h"
#include "elf.h"
#include "heap.h"
#include "string.h"
#include "paging.h"

/*
 * elfSlotBase/elfSlotStackTop/elfSlotBrkStart are pure address arithmetic
 * (no VFS, no scheduler, no paging), so unlike exec()/spawn() themselves
 * (which only run for real via a live boot demo -- see proctest.c and
 * tools/ci-test.sh needles execstart-ok/spawn-wait-ok/exec-pid-preserved-ok)
 * this math is fully coverable here, deterministically, before the
 * scheduler even starts.
 */

KTEST(elfSlotBaseFloorsToSlotBoundary)
{
    KT_EQU(t, elfSlotBase(0x40000000ull), 0x40000000ull);
    KT_EQU(t, elfSlotBase(0x40001000ull), 0x40000000ull);
    KT_EQU(t, elfSlotBase(0x4FFFFFFFull), 0x40000000ull);
    KT_EQU(t, elfSlotBase(0x50000000ull), 0x50000000ull);
    KT_EQU(t, elfSlotBase(0xB0000ABCull), 0xB0000000ull);
}

KTEST(elfSlotStackAndBrkMatchBootTimeConvention)
{
    /* kmain.c's spawnUser calls hardcode stackTop=base+0x08000000 and
       brk=base+0x04000000 for every binary already spawned at boot (e.g.
       sbrktest: base 0x60000000, stackTop 0x68000000, brk 0x64000000) --
       the derived values must agree exactly, since exec()/spawn() reuse
       the same slots and a mismatch would map a fresh stack on top of
       code/data or vice versa. */
    KT_EQU(t, elfSlotStackTop(0x60001000ull), 0x68000000ull);
    KT_EQU(t, elfSlotBrkStart(0x60001000ull), 0x64000000ull);

    KT_EQU(t, elfSlotStackTop(0x40000abcull), 0x48000000ull);
    KT_EQU(t, elfSlotBrkStart(0x40000abcull), 0x44000000ull);

    /* An entry point right at the top of its slot still derives the same
       slot-relative stack/brk as one near the bottom. */
    KT_EQU(t, elfSlotStackTop(0x0FFFFFFFull), 0x08000000ull);
    KT_EQU(t, elfSlotBrkStart(0x0FFFFFFFull), 0x04000000ull);
}

/*
 * elfBuildInitialStack (added alongside argv/argc support for exec()/
 * spawn(): copyUserArgv in syscall.c, crt.S's _start now reading argc/argv
 * off the stack instead of calling main() with nothing) does not care
 * whether the memory it writes into is a real user slot or not -- it just
 * needs [stackTop-stackSize, stackTop) to already be valid, writable
 * memory, which a plain kmalloc buffer satisfies just as well as a
 * pagingMapUserRange'd user stack. That makes the exact byte layout fully
 * testable here, deterministically, without any live process at all --
 * the live proof (kmain.c's proctest.c spawning argtest.c with a specific
 * argv, needles "proctest: spawn with argv ok"/"argtest: argv-ok" in
 * tools/ci-test.sh) only proves crt.S reads the same layout back
 * correctly, not what the layout itself has to look like.
 */

KTEST(elfBuildInitialStackLaysOutArgcArgvAndStringsCorrectly)
{
    uint64 stackSize = 4096;
    uint8* stack = (uint8*)kmalloc(stackSize);
    uint64 stackTop = (uint64)stack + stackSize;

    const char* argv[3] = { "/bin/argtest", "alpha", "beta" };
    uint64 rsp = 0;
    KT_TRUE(t, elfBuildInitialStack(stackTop, stackSize, argv, 3, &rsp));

    /* SysV process entry: rsp must land inside the stack it was given and
       be 16-byte aligned so _start's own `call main` leaves main looking
       at the usual post-call rsp%16==8. */
    KT_GE(t, rsp, (uint64)stack);
    KT_LT(t, rsp, stackTop);
    KT_EQU(t, rsp % 16, 0);

    const uint64* slots = (const uint64*)rsp;
    KT_EQ(t, (int64)slots[0], 3);
    KT_STREQ(t, (const char*)slots[1], "/bin/argtest");
    KT_STREQ(t, (const char*)slots[2], "alpha");
    KT_STREQ(t, (const char*)slots[3], "beta");
    KT_EQU(t, slots[4], 0);

    kfree(stack);
}

KTEST(elfBuildInitialStackWithZeroArgcStillTerminatesArgv)
{
    uint64 stackSize = 256;
    uint8* stack = (uint8*)kmalloc(stackSize);
    uint64 stackTop = (uint64)stack + stackSize;

    uint64 rsp = 0;
    KT_TRUE(t, elfBuildInitialStack(stackTop, stackSize, nullptr, 0, &rsp));
    KT_EQU(t, rsp % 16, 0);

    const uint64* slots = (const uint64*)rsp;
    KT_EQ(t, (int64)slots[0], 0);
    KT_EQU(t, slots[1], 0);

    kfree(stack);
}

KTEST(elfBuildInitialStackRejectsArgcAboveElfMaxArgs)
{
    uint64 stackSize = 4096;
    uint8* stack = (uint8*)kmalloc(stackSize);
    uint64 stackTop = (uint64)stack + stackSize;

    const char* argv[1] = { "x" };
    uint64 rsp = 0;
    KT_FALSE(t, elfBuildInitialStack(stackTop, stackSize, argv, elfMaxArgs + 1, &rsp));
    KT_FALSE(t, elfBuildInitialStack(stackTop, stackSize, argv, -1, &rsp));

    kfree(stack);
}

KTEST(elfBuildInitialStackRejectsArgvTooBigForStack)
{
    /* Deliberately far too small to hold even the fixed argc/argv/NULL
       area for a single argument, let alone the string itself -- must fail
       cleanly (false, *outRsp untouched) rather than write below
       stackTop-stackSize. */
    uint64 stackSize = 8;
    uint8* stack = (uint8*)kmalloc(stackSize);
    uint64 stackTop = (uint64)stack + stackSize;

    const char* argv[1] = { "/bin/argtest" };
    uint64 rsp = 0xDEADBEEFull;
    KT_FALSE(t, elfBuildInitialStack(stackTop, stackSize, argv, 1, &rsp));
    KT_EQU(t, rsp, 0xDEADBEEFull);

    kfree(stack);
}

/*
 * elfSegmentPagingFlags is elfLoad's own per-segment W^X decision reduced
 * to pure arithmetic on a p_flags value -- no paging table, no live
 * process, so every PF_X/PF_W combination a real ELF linker could ever
 * emit is checked here directly, deterministically, rather than only
 * inferred indirectly from whether user/elftest.c's live boot demo (below)
 * happens to fault where expected.
 */

KTEST(elfSegmentPagingFlagsReadExecuteIsWritableNeverAndAlwaysExecutable)
{
    /* A real .text/.rodata PT_LOAD segment: PF_R|PF_X (5). Must come back
       with neither pageWrite nor pageNoExecute -- read-only, executable. */
    KT_EQU(t, elfSegmentPagingFlags(5), 0ull);
}

KTEST(elfSegmentPagingFlagsReadWriteIsWritableAndNeverExecutable)
{
    /* A real .data/.bss PT_LOAD segment: PF_R|PF_W (6). Must come back
       with pageWrite set and pageNoExecute set -- the actual W^X split
       this session's change exists to enforce. */
    KT_EQU(t, elfSegmentPagingFlags(6), pageWrite | pageNoExecute);
}

KTEST(elfSegmentPagingFlagsReadOnlyIsNeitherWritableNorExecutable)
{
    /* PF_R alone (4) -- a real ELF can emit a read-only, non-executable
       segment (e.g. .rodata split into its own PT_LOAD by some linkers,
       even though this kernel's own user.ld keeps it with .text). Must
       still come back non-writable AND non-executable -- PF_X is the only
       bit that ever clears pageNoExecute, never PF_R. */
    KT_EQU(t, elfSegmentPagingFlags(4), pageNoExecute);
}

KTEST(elfSegmentPagingFlagsReadWriteExecuteIsWritableAndExecutable)
{
    /* PF_R|PF_W|PF_X (7) -- every real linker refuses to emit this for a
       normal PT_LOAD, but the function must still do the honest thing
       with it rather than assume it can never happen: writable (PF_W)
       and executable (PF_X), i.e. neither hardening bit set. Confirms
       PF_W and PF_X are decided independently of one another, not "if
       not exactly PF_X, assume data". */
    KT_EQU(t, elfSegmentPagingFlags(7), pageWrite);
}
