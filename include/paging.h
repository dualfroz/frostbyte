#ifndef FROSTBYTE_PAGING_H
#define FROSTBYTE_PAGING_H

#include "types.h"

#define pagePresent 0x1
#define pageWrite   0x2
#define pageUser    0x4

/* Real x86_64 PTE bit 63 (XD/NX) -- refuses instruction fetch against a
   page that has it set, enforced by the CPU itself once
   pagingNxSupported() is true (boot.S enables IA32_EFER.NXE at boot,
   gated on CPUID.80000001H:EDX.NX so this never sets a reserved bit on
   hardware/hypervisors that predate it). pagingMap silently drops this
   bit from the leaf entry it writes when NX is not supported -- the page
   stays executable rather than risk a reserved-bit #PF, an honest
   best-effort rather than a promise this bit is always enforced. */
#define pageNoExecute 0x8000000000000000ull

/* True once boot.S has confirmed both that the CPU reports
   CPUID.80000001H:EDX.NX and that IA32_EFER.NXE was actually set --
   callers that care whether pageNoExecute is real hardware enforcement
   (as opposed to a bit pagingMap quietly ignored) check this. */
bool pagingNxSupported(void);

bool pagingMap(uint64 virtualAddr, uint64 physicalAddr, uint64 flags);
uint64 pagingResolve(uint64 virtualAddr);

/* True if virtualAddr is currently mapped AND executable (present, and
   either pageNoExecute was never asked for or the CPU cannot enforce it
   -- see pagingNxSupported). False for an unmapped address or one mapped
   with pageNoExecute successfully applied. Introspection only, used by
   pagingTest.c/mmapTest.c to verify pagingMap/pagingMapUserRangeFlags
   actually wrote the bit they were asked to, without needing a live
   instruction-fetch fault to prove it indirectly. */
bool pagingIsExecutable(uint64 virtualAddr);

/* Same shape as pagingIsExecutable, for pageWrite instead of pageNoExecute:
   true if virtualAddr is currently mapped AND writable, false for an
   unmapped address or one mapped without pageWrite. Introspection only,
   added alongside pagingMapUserRangeExact so pagingTest.c can prove a
   range came back genuinely read-only (e.g. an ELF binary's own .text/
   .rodata, elf.c's elfLoad) directly, without needing a live user-mode
   write fault to prove it indirectly the way user/elfwritevictim.c's
   SIGSEGV does. */
bool pagingIsWritable(uint64 virtualAddr);

bool pagingMapUserRange(uint64 virtualAddr, uint64 size);

/* Same as pagingMapUserRange, plus extraFlags (pageNoExecute, currently
   the only bit worth passing here) OR'd into every leaf PTE this call
   creates -- pagingMapUserRange itself is a thin wrapper passing 0.
   Callers that want a non-executable range (a stack, a brk-grown heap --
   see kmain.c/syscall.c/jobctl.c's stack setup and sched.c's
   currentThreadSbrk) pass pageNoExecute. A thin wrapper itself, over
   pagingMapUserRangeExact below, that always forces pageWrite on --
   every caller of this entry point (the ones just named, plus
   pagingMapUserRange) wants a writable range and only ever varies whether
   it is also executable. elf.c's own per-ELF-segment loader is the one
   caller in this kernel that needs a range which may be READ-ONLY too (a
   binary's own .text/.rodata), so it calls pagingMapUserRangeExact
   directly instead. */
bool pagingMapUserRangeFlags(uint64 virtualAddr, uint64 size, uint64 extraFlags);

/* Lowest-level of the three pagingMapUserRange* entry points: flags is
   the exact set of extra leaf PTE bits (pageWrite, pageNoExecute) OR'd
   onto pageUser for every page in the range -- unlike
   pagingMapUserRangeFlags, pageWrite is NOT forced on, so a caller that
   omits it gets a genuinely read-only mapping. elf.c's elfLoad is the
   only caller today: each ELF program header's own p_flags (via
   elfSegmentPagingFlags) decides per-segment whether the range this call
   maps is writable, executable, both, or (read-only, non-executable)
   neither -- real per-segment W^X instead of one blanket writable+
   executable range for a whole binary's image. */
bool pagingMapUserRangeExact(uint64 virtualAddr, uint64 size, uint64 flags);

/* Inverse of pagingMapUserRange: walks [virtualAddr, virtualAddr+size) one
   frame at a time and, for every page that is actually present, gives its
   physical frame back to the pmm and clears the PTE (present bit gone,
   TLB invalidated). Pages that were never mapped -- including whole page-
   table levels that were never allocated -- are silently skipped rather
   than treated as an error: callers (threadReap/threadDiscard in sched.c,
   doExec in syscall.c) always unmap a thread's whole elfSlotSize-sized
   slot, most of which a typical small user program never touches. Safe to
   call on a range that overlaps no mapping at all (a no-op). */
void pagingUnmapUserRange(uint64 virtualAddr, uint64 size);

#endif
