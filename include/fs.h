#ifndef FROSTBYTE_FS_H
#define FROSTBYTE_FS_H

#define oReadOnly  0x0
#define oWriteOnly 0x1
#define oReadWrite 0x2
#define oCreate    0x40
#define oTrunc     0x200
#define oAppend    0x400

#define seekSet 0
#define seekCur 1
#define seekEnd 2

#define fsTypeFile 1
#define fsTypeDir  2
#define fsTypeFifo 3

/* Real POSIX permission bits (chmod(2)/open(2)'s access check). This kernel
   now has a real (if minimal) uid AND gid model -- sched.h's Thread.uid/
   Thread.gid, vfs.c's vfsPermitted -- so all three (owner/group/other)
   triads are genuinely stored AND enforced. A mode value like 0750 is
   accepted and echoed back by vfsStat/userStat exactly as given. Enforced
   at exactly two real choke points, both uid+gid-aware via vfsPermitted:
   resolveOpen (vfs.c) refuses to open an existing file for writing without
   the right write bit for the CALLING thread's identity, and
   loadImageByPath/jobctlSpawn (syscall.c/jobctl.c) refuse to exec a file
   without the right exec bit -- both real, both checked at open/spawn time
   only, never re-checked against an fd/process already granted (matching
   real Unix). Deliberately no uid-0-bypasses-everything superuser rule
   like real Unix's root has -- see vfs.c's vfsPermitted doc comment for
   why a from-scratch bypass would have silently broken every permission
   test already relying on chmod denying write even to a file's own
   creator (every boot-time process starts at uid 0). */
#define fsModeOwnerRead  0400u
#define fsModeOwnerWrite 0200u
#define fsModeOwnerExec  0100u

/* The "group" triad -- vfsPermitted's middle fallback, consulted once a
   caller's uid fails to match a node's owner uid but its GID (sched.h's
   Thread.gid) matches the node's own group gid (fs.h's FbStat.gid). Each
   bit is its own owner bit shifted right by 3 (real Unix's own rwxrwxrwx
   bit layout: owner at bits 8-6, group at bits 5-3, other at bits 2-0),
   the same derivation fsModeOther* below already used for the next shift
   down -- vfsPermitted computes both from wantOwnerBit rather than storing
   six separate constants per permission. */
#define fsModeGroupRead  0040u
#define fsModeGroupWrite 0020u
#define fsModeGroupExec  0010u

/* The "other" (neither owner nor group) triad -- vfsPermitted's final
   fallback once a caller's uid AND gid both fail to match. Numerically each
   is its own owner bit shifted right by 6, matching the 6-bit gap between
   the owner and other triads in a real Unix mode. */
#define fsModeOtherRead  0004u
#define fsModeOtherWrite 0002u
#define fsModeOtherExec  0001u

/* Defaults applied at creation time (never retroactive -- an existing
   file's mode never changes on its own, only through chmod). Directory
   mode bits are stored and reported by stat/ls -l but never enforced: this
   kernel's existing per-node `writable` (tmpfs) / dir-emptiness (SFS) gates
   already govern directory mutation, a second, redundant enforcement axis
   was not worth the complexity for a case no test needs. */
#define fsModeDefaultFile 0644u
#define fsModeDefaultDir  0755u
#define fsModeDefaultExec 0755u

/* sysMmap's `prot` argument (ulib.h's userMmap) -- POSIX PROT_NONE/READ/
   WRITE/EXEC by shape. mmapProtExec IS real and hardware-enforced
   (mmapFaultHandler, mmap.c, maps every page of a Vma missing this bit
   with paging.h's pageNoExecute): jumping into a mapping that never asked
   for it takes a real, catchable SIGSEGV (isr.c's I/D-bit short-circuit in
   tryResolveUserPageFault), never silently succeeds -- WHEN
   paging.h's pagingNxSupported() is true (boot.S enables IA32_EFER.NXE
   only after confirming CPUID.80000001H:EDX.NX; unsupported hardware
   silently keeps every mapping executable rather than write a reserved PTE
   bit, the same honest best-effort this codebase already applies
   elsewhere). This mechanism is not limited to mmap: elf.c's own loader
   now maps each ELF program header with the same paging.h flags, derived
   from that segment's own p_flags (elfSegmentPagingFlags) -- a binary's
   .text/.rodata comes up read-only+executable, its .data/.bss read-write+
   non-executable (user/user.ld emits them as two separate PT_LOAD
   segments precisely so they can get different flags), and the
   fixed-size stack and brk heap (kmain.c/syscall.c/jobctl.c/sched.c) still
   ask pagingMapUserRangeFlags for pageNoExecute explicitly the same way
   they always have. mmapProtWrite IS also real and enforced
   (mmapFaultHandler, mmap.c): a write fault against a mapping missing this
   bit is refused and turns into a real SIGSEGV (isr.c), never silently
   allowed -- the same real enforcement elf.c's own read-only .text/.rodata
   segments now get too (user/elfwritevictim.c). */
#define mmapProtNone  0x0
#define mmapProtRead  0x1
#define mmapProtWrite 0x2
#define mmapProtExec  0x4

/* sysMmap's `flags` argument -- MAP_ANONYMOUS-equivalent: fd/offset are
   ignored and every page is demand-zero-filled instead of read from a file
   (mmap.c's mmapFaultHandler). There is no MAP_SHARED bit: every mapping
   here behaves like MAP_PRIVATE (a file-backed page's own frame is this
   mapping's alone -- writing to it, when mmapProtWrite allows that, never
   writes back to the underlying file, exactly like a real MAP_PRIVATE
   mapping's own copy-on-write semantics, just without ever actually
   sharing the frame with the file cache to begin with, since this kernel
   has none), so the file-backed default already gets MAP_PRIVATE's own
   behavior without a second flag bit to ask for it. */
#define mmapAnonymous 0x1

#define fsNameMax 64

/* Shared cap for a full path string (as opposed to fsNameMax's single
   component): sched.h's Thread.cwd, vfs.h's vfsResolvePath/vfsGetcwd, and
   every syscall.c path-copying stack buffer (sysOpen/sysMkdir/sysChdir/...)
   all use this same constant, so a path that fits one always fits the
   other -- shell.c's own longstanding local pathCapacity used the same 256
   value before this constant existed. */
#define fsPathMax 256

typedef struct FbStat {
    unsigned long size;
    unsigned int type;
    /* Real owner rwx permission bits (see fsModeOwner* above) for a tmpfs
       or SFS node -- a synthetic 0666/0444 derived from `writable` before
       chmod(2) existed, now the actual per-node value. Always 0666 for
       /dev/disk0 (no real per-file mode concept for a raw block device,
       an honest, never-enforced placeholder, unlike the file/dir case). */
    unsigned int mode;
    /* Real owner identity (POSIX uid): the sched.h Thread.uid value that
       was current at creation time for a tmpfs or SFS node -- see vfs.c's
       vfsUid/sfs.h's sfsUidOf, the same accessors vfsPermitted itself reads
       to decide owner-vs-other access. Never retroactive at creation time,
       like mode's own default, and never changed by chmod -- only a real
       vfsChown (root-only, real Unix's _POSIX_CHOWN_RESTRICTED default) can
       reassign it afterward. Always 0 ("root") for /dev/disk0 and the in-memory
       tmpfs root itself, mirroring mtime/ctime's own "0 means never
       tracked" placeholder immediately below for cases with no real
       creation event of their own. */
    unsigned int uid;
    /* Real group identity (POSIX gid): the sched.h Thread.gid value that was
       current at creation time for a tmpfs or SFS node -- see vfs.c's
       vfsGid/sfs.h's sfsGidOf, the same accessors vfsPermitted itself reads
       to decide the group branch of owner-vs-group-vs-other access. Never
       retroactive at creation time, like uid's own default, and never
       changed by chmod -- only vfsChgrp (owner may only set it to their OWN
       current gid, root may set it to anything, see vfs.c's vfsChgrp doc
       comment) can reassign it afterward. Always 0 for /dev/disk0 and the
       in-memory tmpfs root itself, mirroring uid's own placeholder. */
    unsigned int gid;
    /* Real wall-clock timestamps (seconds since the Unix epoch, rtc.h's
       rtcNow()) for an SFS ("/mnt") inode -- see sfs.h's sfsMtimeOf/
       sfsCtimeOf. Always 0 for the in-memory tmpfs tree and /dev/disk0,
       which have never tracked time (documented gap, not a faked value). */
    unsigned long mtime;
    unsigned long ctime;
} FbStat;

typedef struct FbDirent {
    unsigned int type;
    unsigned int nameLength;
    char name[fsNameMax];
} FbDirent;

#endif
