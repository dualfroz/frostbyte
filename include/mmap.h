#ifndef FROSTBYTE_MMAP_H
#define FROSTBYTE_MMAP_H

#include "types.h"
#include "fs.h"

struct Thread;

/* Cap on live mappings per thread. Small and fixed like every other
   per-Thread table in this kernel (maxOpenFiles=16, sigMaxSignals=32) --
   nothing here needs more than a handful of distinct mappings at once, and
   a hard cap keeps Vma[] a plain fixed-size array embedded directly in
   Thread (sched.h), exactly like FileDescriptor fds[maxOpenFiles] already
   is, rather than something that needs its own allocator. */
#define maxVmasPerThread 8

/* One live mapping inside a single thread's own elfSlotSize slot (see
   elf.h) -- mmap.c reserves [start, end) at mmapReserve time but allocates
   no physical frames and reads no file data yet; mmapFaultHandler below
   fills each page lazily, the first time a real #PF (isr.c) proves it was
   actually touched. `fd` (meaningful only when `anonymous` is false) names
   a slot in the OWNING thread's own fd table (vfs.h) -- this mapping is
   only ever resolved for its own owning thread (mmapFaultHandler's own
   contract), so there is no cross-thread fd ambiguity. Unlike a real
   kernel's struct vm_area_struct, there is no backing inode reference
   independent of that fd: closing `fd` before every page has been faulted
   in leaves this mapping unable to fill the rest (mmapFaultHandler treats
   a since-closed fd as a fault it cannot resolve, turning into a real
   SIGSEGV rather than silently serving zeroed pages for wrong reasons) --
   an honestly scoped limitation (real mmap(2) lets a mapping outlive the
   fd that created it), not a hidden bug, chosen because holding a
   reference to a file independent of any thread's fd table would need a
   refcounted VfsNode/inode layer this kernel does not otherwise have. */
typedef struct Vma {
    bool used;
    uint64 start;
    uint64 end;
    uint32 prot;
    bool anonymous;
    int fd;
    uint64 fileOffset;
} Vma;

/* Reserves [addr, addr+length) -- or, when addr is 0, a first-fit region
   of `length` bytes the kernel picks itself, real mmap(NULL, ...)-style --
   inside `thread`'s own mmap region ([userSlotBase+elfSlotStackOffset,
   userSlotBase+elfSlotSize), see elf.h: the 128 MiB of every elfSlotSize
   slot that sits above the small fixed-size stack and is never otherwise
   touched by an ELF image or brk). No physical frame is allocated and no
   byte is read yet -- see mmapFaultHandler for when that actually happens.
   On success, writes the mapping's real base address to *outAddr (matters
   when addr was 0) and returns true. Returns false, touching *outAddr not
   at all, if: length is 0, addr or length is not frameSize-aligned, a
   non-anonymous mapping's fileOffset is not frameSize-aligned, the
   resulting range would fall outside the mmap region, it would overlap an
   existing Vma of this same thread, no addr was given and no free region
   of that size exists, or every one of maxVmasPerThread's own slots is
   already in use. */
bool mmapReserve(struct Thread* thread, uint64 addr, uint64 length, uint32 prot,
                  bool anonymous, int fd, uint64 fileOffset, uint64* outAddr);

/* Real munmap(2): removes every Vma of `thread` covering any part of
   [addr, addr+length), giving back to the pmm whatever physical frames
   were actually faulted in for the removed range (pagingUnmapUserRange,
   paging.h -- the same "only what is actually present costs anything"
   contract that function already documents for itself). A length that only
   overlaps one end of an existing Vma shrinks it in place; a length that
   lands entirely inside one Vma's interior, touching neither end, splits
   it into two (consuming one more Vma slot for the upper half) -- since
   Vmas of the same thread never overlap each other (mmapReserve's own
   refusal), at most one Vma can ever need splitting per call, so this
   either finds the one free slot it needs or refuses the WHOLE call up
   front, leaving every Vma of this thread completely untouched, rather
   than leaving the caller's mapping table half-modified by a resource
   failure it could not have predicted from addr/length alone. A range that
   overlaps no Vma of this thread at all is a no-op returning true, real
   munmap(2)'s own "unmapping already-unmapped memory is not an error"
   behavior. Returns false only for a misaligned addr, a zero length, or
   the above split-with-no-free-slot case. */
bool mmapRelease(struct Thread* thread, uint64 addr, uint64 length);

/* Looks up the (at most one, since a thread's own Vmas never overlap each
   other) Vma of `thread` covering `addr`, or nullptr if none does. */
Vma* mmapFind(struct Thread* thread, uint64 addr);

/* The real page-fault resolver: called by isr.c for a ring-3 #PF whose
   faulting address might belong to `thread`'s own mmap region. Looks up
   the covering Vma (mmapFind); returns false, mapping nothing, if there is
   none, or if `forWrite` is true but the Vma's own prot lacks
   mmapProtWrite (isr.c turns either case into a real SIGSEGV, see its own
   comment) or (mmapProtRead is missing on a read fault, refused the same
   way). Otherwise allocates one physical frame, fills it -- zeroed for an
   anonymous Vma; for a file-backed one, read from the Vma's own `fd`
   starting at the file offset that corresponds to this page (whatever
   vfsRead returns short of a full frameSize read, e.g. a page straddling
   end-of-file, is left zero-padded, since the frame was already
   zero-filled before the read) -- maps it at the containing page with
   pageUser | (mmapProtWrite set ? pageWrite : 0), and returns true so
   isr.c can simply retry the faulting instruction, exactly like any other
   successfully resolved demand-paging fault. Only ever meaningful when
   `thread` is the CURRENTLY RUNNING thread (it reads/restores that
   thread's own fd offset through vfsRead/vfsLseek, which resolve fd
   numbers against currentThread() -- true for both isr.c's real caller and
   mmapTest.c's own KTEST coverage, which runs entirely on the idle thread,
   itself always currentThread() while KTEST executes). */
bool mmapFaultHandler(struct Thread* thread, uint64 faultAddr, bool forWrite);

/* Zeroes every Vma of `thread` without touching any physical frame or
   page-table entry -- callers that already reclaim frames some other way
   (doExec's own pagingUnmapUserRange over the relevant range, syscall.c)
   call this afterward purely to stop a later mmapReserve on the same
   thread from ever finding a stale Vma left over from an image this
   thread no longer runs. threadReap/threadDiscard (sched.c) do NOT need
   to call this themselves: both already memset() the whole Thread struct
   before a slot is handed back to allocSlot, which zeroes the embedded
   Vma[] array as a side effect, exactly like every other per-thread table
   in it. */
void mmapReleaseAll(struct Thread* thread);

#endif
