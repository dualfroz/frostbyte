#ifndef FROSTBYTE_ELF_H
#define FROSTBYTE_ELF_H

#include "types.h"

typedef struct Elf64Header {
    uint8 ident[16];
    uint16 type;
    uint16 machine;
    uint32 version;
    uint64 entry;
    uint64 programHeaderOffset;
    uint64 sectionHeaderOffset;
    uint32 flags;
    uint16 headerSize;
    uint16 programHeaderEntrySize;
    uint16 programHeaderCount;
    uint16 sectionHeaderEntrySize;
    uint16 sectionHeaderCount;
    uint16 sectionNameIndex;
} __attribute__((packed)) Elf64Header;

typedef struct Elf64ProgramHeader {
    uint32 type;
    uint32 flags;
    uint64 offset;
    uint64 virtualAddr;
    uint64 physicalAddr;
    uint64 fileSize;
    uint64 memorySize;
    uint64 align;
} __attribute__((packed)) Elf64ProgramHeader;

uint64 elfLoad(const void* image, uint64 imageSize);

/* Maps one ELF64 program header's p_flags (segmentFlags: standard PF_X=1/
   PF_W=2/PF_R=4 bit values) onto the extra paging.h leaf-PTE bits
   (pageWrite, pageNoExecute) elfLoad passes to pagingMapUserRangeExact for
   that segment -- real per-segment W^X (a binary's own .text/.rodata ends
   up read-only+executable, its .data/.bss read-write+non-executable)
   instead of one blanket writable+executable range for the whole image.
   Pure address-flag arithmetic, no paging/scheduler/VFS state touched, so
   it is fully unit-testable (elfTest.c) the same way elfSlotBase and
   friends already are. */
uint64 elfSegmentPagingFlags(uint32 segmentFlags);

/* Every userspace binary in this kernel is linked (Makefile --defsym
   USER_BASE=...) at a fixed, elfSlotSize-aligned base of its own, since
   there is one shared address space system-wide (no per-process page
   tables/CR3) and no relocation support (-fno-pic): two binaries must
   simply never overlap. kmain.c's boot-time spawnUser calls hardcode each
   binary's stack/brk address following the same base+offset pattern by
   hand; elfSlotStackTop/elfSlotBrkStart let syscall.c's exec()/spawn()
   (a path resolved at runtime, not known at kmain.c's compile time)
   derive the same addresses purely from the ELF's own entry point. */
#define elfSlotSize        0x10000000ull
#define elfSlotStackOffset 0x08000000ull
#define elfSlotBrkOffset   0x04000000ull

uint64 elfSlotBase(uint64 entry);
uint64 elfSlotStackTop(uint64 entry);
uint64 elfSlotBrkStart(uint64 entry);

/* Ceiling on argv entries any single exec()/spawn()/boot launch can pass
   (syscall.c's copyUserArgv enforces the same bound while copying them out
   of userspace). Chosen together with the 256-byte per-argument cap
   syscall.c already uses for path-like strings so that
   elfMaxArgs*256 + a few hundred bytes of pointers/argc never comes close
   to execStackSize (syscall.c) or kmain.c's userStackSize -- both far
   larger than any realistic argv list this kernel's own binaries pass. */
#define elfMaxArgs 16

/* Lays out a process's very first stack frame the way a real x86_64 SysV
   process entry looks (used by user/crt.S's _start): from stackTop down,
   the argument strings themselves, then (16-byte aligned per the ABI)
   argc, argv[0..argc-1], and a NULL argv terminator -- *outRsp is where
   _start should start executing with, pointing at argc.
   argv/argc are already kernel-side data (copyUserArgv, doExec/doSpawn in
   syscall.c, or a literal array for kmain.c's boot spawnUser) by the time
   this runs; this function only ever writes into [stackTop-stackSize,
   stackTop), which the caller must have already mapped (pagingMapUserRange)
   -- same contract as elfLoad's segment writes.
   Returns false, writing nothing, if argc is out of [0, elfMaxArgs] or the
   strings plus the fixed-size argv/argc area do not fit in stackSize bytes
   -- unreachable for every call site in this kernel today given the bound
   above, but checked rather than assumed so a future caller with a bigger
   argv (or a deliberately tiny stack in elfTest.c) fails cleanly instead of
   silently writing before stackTop-stackSize. */
bool elfBuildInitialStack(uint64 stackTop, uint64 stackSize, const char* const* argv,
                           int argc, uint64* outRsp);

#endif
