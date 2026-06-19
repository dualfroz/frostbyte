#ifndef FROSTBYTE_VFS_H
#define FROSTBYTE_VFS_H

#include "types.h"
#include "fs.h"

struct VfsNode;

typedef enum {
    fdFree = 0,
    fdConsole = 1,
    fdFile = 2,
    fdPipeRead = 3,
    fdPipeWrite = 4,
    fdBlockDev = 5,
    fdSfs = 6,
    /* pidfd.h handle wrapped as an ordinary fd (FileDescriptor.pipeHandle
       reused to store the pidfd.c table index, same field-reuse convention
       fdPipeRead/fdPipeWrite already use for a pipe.c handle) -- unlike
       anonymous pipes, a pidfd never needs the cross-process
       pipeCreateDetached/vfsPipeAttach dance: it is opened by, and lives
       entirely within, the SAME process that calls vfsPidfdOpen, exactly
       like real Linux's pidfd_open(2). */
    fdPidfd = 7
} FdKind;

/* FileDescriptor.flags bit for a pipe end installed by vfsPipeAttach: reads/
   writes on it should really block (via syscall.c's sysRead/sysWrite retry)
   rather than report "would block" (-1) the way vfsPipe's anonymous,
   single-process pipe ends always have (see pipetest.c, which depends on
   that -1 -- unflagged, unchanged). File-kind descriptors already store
   their vfsOpen flags (oReadOnly etc, fs.h) in this same field, but never a
   pipe fd, so the two uses never collide. */
#define fdFlagBlocking 0x1

typedef struct FileDescriptor {
    struct VfsNode* node;
    uint64 offset;
    uint32 kind;
    uint32 flags;
    int32 pipeHandle;
} FileDescriptor;

#define maxOpenFiles 16

void vfsInit(void);
void vfsMountUstar(uint64 start, uint64 end);
void vfsMountTmpfs(const char* path);
void vfsFdInit(FileDescriptor* fds);

int vfsOpen(const char* path, uint32 flags);
int vfsClose(int fd);
int64 vfsRead(int fd, void* buffer, uint64 length);
int64 vfsWrite(int fd, const void* buffer, uint64 length);
int64 vfsLseek(int fd, int64 offset, int whence);
int vfsStat(const char* path, FbStat* out);
int64 vfsGetdents(int fd, void* buffer, uint64 length);
int vfsMkdir(const char* path);
int vfsUnlink(const char* path);
int vfsRmdir(const char* path);

/* Real chmod(2)/chown(2) privilege rule, factored out as a pure predicate
   exactly like vfsPermitted itself (see that function's own doc comment for
   why): true if `callerUid` may change `ownerUid`-owned metadata, i.e. is
   either the owner itself or uid 0 ("root", administrative bypass -- unlike
   vfsPermitted's read/write/exec check, which deliberately has none). Only
   chmod uses this; chown is stricter still (root-only, see vfsChown) since
   real Unix's default _POSIX_CHOWN_RESTRICTED denies even a file's own
   owner the right to give it away. Pure and explicit-argument like
   vfsPermitted so it stays testable without needing a live thread's own
   uid -- KTEST always runs on the idle thread (permanently uid 0), which
   could never otherwise exercise the "denied" side of this check. */
bool vfsOwnerOrRoot(uint32 ownerUid, uint32 callerUid);

/* Real chmod(2): overwrites `path`'s whole owner-rwx mode (fs.h's
   fsModeOwner*) with `mode` -- no symbolic +x-style merge, matching real
   chmod's own numeric-mode form. Refuses unless the caller owns the file or
   is uid 0 (vfsOwnerOrRoot above) -- a real, previously-missing check: every
   uid could chmod any file's bits until this was added. Dispatches on
   isSfsPath like every other entry point above (sfsChmod for "/mnt", the
   tmpfs VfsNode's own `mode` field otherwise). Returns -1 for a missing
   path, or (tmpfs only) a node under a non-writable mount -- chmod is a
   metadata mutation, gated by the exact same `writable` flag every other
   tmpfs mutation (vfsUnlink, vfsWrite's write path) already respects, so a
   ustar-loaded /bin/ *.elf file can never have its mode changed at runtime,
   matching its read-only backing store. 0 on success. */
int vfsChmod(const char* path, uint32 mode);

/* Real chown(2): overwrites `path`'s owner uid outright. Root-only
   (currentThreadUid() == 0), full stop -- unlike vfsChmod, ownership is
   never delegated to the file's own owner (real Unix's
   _POSIX_CHOWN_RESTRICTED default: an owner who could give a file away
   could use that to defeat disk-quota accounting, so only root may). Same
   isSfsPath dispatch and missing-path/non-writable-mount refusal as
   vfsChmod. 0 on success. */
int vfsChown(const char* path, uint32 newUid);

/* Real chgrp(2): overwrites `path`'s group gid. Unlike vfsChown (root-only,
   full stop), chgrp is delegated to the file's own owner too -- but only to
   set the group to the CALLER's own current gid (currentThreadGid()), never
   an arbitrary one: this kernel tracks exactly one gid per thread (no
   supplementary-groups list), so "a group the caller belongs to" collapses
   to "the caller's own current gid", matching real chgrp(2)'s own
   membership requirement for a non-privileged caller. Root (uid 0) may set
   any gid, same administrative bypass vfsOwnerOrRoot already grants chmod.
   Same isSfsPath dispatch and missing-path/non-writable-mount refusal as
   vfsChown. 0 on success. */
int vfsChgrp(const char* path, uint32 newGid);

/* Symlinks and hardlinks are only supported on the persistent "/mnt"
   filesystem (sfs.h already has the on-disk machinery for both); any other
   path (the ustar/tmpfs VfsNode tree, "/dev/disk0") rejects these with -1,
   the same way a real filesystem that lacks the feature would refuse the
   syscall rather than fake it. */
int vfsSymlink(const char* path, const char* target);
int64 vfsReadlink(const char* path, char* buffer, uint64 cap);
int vfsLink(const char* existingPath, const char* newPath);

/* Creates an anonymous pipe and installs its two ends into the current
   thread's fd table. On success readFd/writeFd receive the descriptors and
   0 is returned; on failure (fd table or pipe table exhausted) -1 is
   returned and neither output is written. */
int vfsPipe(int* readFd, int* writeFd);

/* Creates a pipeCreateDetached handle (pipe.h) with neither end attached to
   anyone's fd table and returns its raw number, or -1 on exhaustion. Meant
   to be handed to two independently spawned processes (typically as a
   decimal argv string, see user/fifomaster.c) so they can rendezvous over a
   real blocking pipe without needing fork() to share an fd -- see
   vfsPipeAttach. */
int vfsPipeCreateHandle(void);

/* Installs `handle` (from vfsPipeCreateHandle, or in principle any live
   pipe handle) into a fresh slot in the CALLING thread's own fd table, as a
   read end (wantWrite false) or write end (wantWrite true), flagged
   fdFlagBlocking so syscall.c's sysRead/sysWrite give it real blocking
   semantics. Returns the new fd, or -1 if the handle is not a live pipe or
   the fd table is full. */
int vfsPipeAttach(int handle, bool wantWrite);

/* Same as vfsPipeAttach, but installs into fd slot `fd` of an EXPLICIT fd
   table instead of allocating a free slot in the CALLING thread's own
   table -- jobctl.c's own way to bake a real pipe end into a brand new
   child's stdin/stdout (fd 0/1) before that child has run a single
   instruction, so there is never a window where it could be scheduled with
   its default console fds instead (see jobctlSpawnPiped, jobctl.h). `fd` is
   trusted to already name a valid slot (0/1/2) in `targetFds` -- unlike
   vfsPipeAttach, this never allocates a fresh one, so the caller decides
   exactly which fd is overwritten. Silently does nothing if `handle` is not
   a live pipe. */
void vfsPipeAttachInto(FileDescriptor* targetFds, int fd, int handle, bool wantWrite);

/* Opens a pidfd (pidfd.h) on `targetId` and installs it as a brand new
   fdPidfd descriptor in the CALLING thread's own fd table -- the
   fd-table-wrapping half of pidfd.h's raw pidfdOpen, exactly the same
   split vfsPipe/vfsPipeAttach already have over pipe.c's raw
   pipeCreate/pipeCreateDetached. Returns the new fd, or -1 if `targetId`
   is not a valid, currently-live thread (pidfdOpen's own contract) or the
   calling thread's own fd table is full. */
int vfsPidfdOpen(uint32 targetId);

/* Resolves fd `fd` (which must be an fdPidfd this thread's own vfsPidfdOpen
   returned) to the live target thread id it still safely refers to, or
   threadInvalidId if `fd` is not an open pidfd at all, or the original
   target has since been reaped/recycled (pidfdTarget's own contract,
   pidfd.h) -- the single check syscall.c's sysPidfdSend/sysPidfdWait both
   need before ever touching a target by number. */
uint32 vfsPidfdTarget(int fd);

/* Same as vfsOpen, but installs into fd slot `fd` of an EXPLICIT fd table
   instead of allocating a free slot in the CALLING thread's own table --
   the file-redirection counterpart of vfsPipeAttachInto above, used by
   jobctl.c's own jobctlApplyRedirects to bake a real file open (not a
   pipe) into a freshly spawned child's stdin/stdout for shell.c's new
   `cmd < file` / `cmd > file` / `cmd >> file` syntax, before that child has
   run a single instruction. `fd` is trusted to already name a valid slot
   (0/1/2) in `targetFds`, exactly like vfsPipeAttachInto. Returns 0 on
   success, -1 on the same failures vfsOpen itself can have (missing path
   without oCreate, unwritable parent directory, a fifo opened with a bad
   mode, exhausted pipe/inode resources) -- leaves `targetFds[fd]`
   completely untouched on failure, the same "never marks a fd used when it
   fails" contract vfsOpen already has for its own newly allocated fd. */
int vfsOpenInto(FileDescriptor* targetFds, int fd, const char* path, uint32 flags);

/* nullptr unless `fd` is a fdFlagBlocking pipe/fifo end or a console (fd 0)
   read whose next read/write would have to report "try again" for a
   resolvable reason (a pipe buffer empty but a writer could still show up,
   full but a reader could still drain it, or a console queue simply not
   having a keystroke queued yet) -- in which case it is the channel
   (sched.h) that end's condition changing will wake. syscall.c's
   sysRead/sysWrite call this after a -1 from doRead/doWrite to decide
   whether to park-and-retry the syscall (schedBlockCurrent + rewind rip,
   same trick sysWait uses for threadJoin) or simply hand -1 back to
   userspace as-is (a non-blocking pipe end, a console write, or a
   permanently broken/at-EOF pipe that retrying could never fix). */
const void* vfsBlockingChannel(int fd, bool forWrite);

/* poll.h's sysPoll (syscall.c): fills *outRevents with the subset of
   `events` (poll.h's pollIn/pollOut) `fd` could satisfy right now without
   blocking -- pollNval instead if `fd` is not open at all, pollErr for a
   pipe write end whose only reader has gone away (a real write there would
   fail immediately, not block, so it counts as "ready" the same way a
   readable EOF pipe does). A regular file, "/dev/disk0", or "/mnt" fd
   always reports every requested bit ready, matching real poll(2): none of
   this kernel's local-storage reads/writes ever return "would block".
   Returns true iff *outRevents ended up nonzero, i.e. this fd alone would
   already satisfy a poll call. */
bool vfsPollReady(int fd, int16 events, int16* outRevents);

/* Named pipe: creates a real filesystem entry of type fsTypeFifo at `path`
   (must live under a writable directory, e.g. the "/tmp" tmpfs mount --
   same requirement makeChildFile already has for oCreate) backed by a fresh
   pipeCreateDetached handle. Unlike vfsPipeCreateHandle/vfsPipeAttach, which
   need SOME cooperating parent to pass the raw handle number to both sides
   (typically as an argv string, see user/fifomaster.c), a fifo node lets any
   two processes that simply agree on a path -- no common ancestor, no
   handle to pass -- rendezvous by both calling vfsOpen on it: whichever
   opens with oWriteOnly gets a blocking write end, whichever opens with
   oReadOnly gets a blocking read end, exactly like vfsPipeAttach's fds.
   oReadWrite (or any other flag combination) is rejected -- this kernel's
   pipe end is inherently one-directional, so a dual-direction fifo fd is
   out of scope, same as vfsPipeAttach's wantWrite being a single bool.
   Returns 0 on success, -1 if the path already exists, its parent is not a
   writable directory, or the pipe table is exhausted. Not supported under
   "/dev/disk0".

   Under "/mnt" this creates a REAL, persistent on-disk entry (sfs.h's
   sfsTypeFifo) that survives sfsUnmount()+sfsMount() -- only the special
   file itself persists, never any buffered bytes (exactly like a real
   mkfifo(2) special file survives a reboot while its pipe buffer does
   not): the live pipe.h handle two openers actually exchange bytes
   through is created lazily, in memory, the first time anyone opens the
   path after boot (or after every previous session's ends have gone away),
   same mechanism as the "/tmp"-style case above, just keyed by inode
   number instead of living on a VfsNode (see vfs.c's sfsFifoHandleFor). */
int vfsMkfifo(const char* path);

/* "/dev/disk0" is a reserved path (not part of the mounted tree, same as
   pipes) that opens the raw ATA disk as a byte-addressable device: reads
   and writes at arbitrary offsets are translated to sector I/O through
   ata.h, with read-modify-write for accesses that are not sector-aligned. */

/* "/mnt" (and everything under it) is a second reserved path, backed by the
   persistent on-disk filesystem in sfs.h/sfs.c rather than the kmalloc-based
   VfsNode tree: open/read/write/close/lseek/stat/mkdir/unlink/getdents on a
   "/mnt..." path are all dispatched to sfs.c, which stores the data on the
   same ATA disk (a different LBA range than /dev/disk0's raw byte access,
   see sfs.h). Data written there survives vfsInit() being called again,
   because it lives on disk, not in this process's heap. */

/* Real per-process chdir(2)/getcwd(2) (sched.h's Thread.cwd). Resolves
   `path` against the CALLING thread's current cwd first (vfsResolvePath
   below), so a relative argument works exactly like a real chdir() call,
   then requires the result to already name a real directory -- under
   "/mnt" (sfs.h) or in the ordinary VfsNode tree, checked the same way
   vfsStat/vfsMkdir already dispatch on isSfsPath. Returns 0 on success (the
   calling thread's own cwd is now that resolved absolute path) or -1,
   changing nothing, if it does not name an existing directory. For a
   VfsNode-tree directory (ustar/tmpfs), the stored result is canonicalized
   by walking the resolved node's real parent chain (vfsPath below), so
   "."/".." components the caller typed never survive into a later
   vfsGetcwd(). "/mnt" (sfs.h) has no equivalent parent-walk primitive, so
   THAT branch stores vfsResolvePath's own plain string join verbatim --
   every path-taking entry point below already tolerates embedded "."/".."
   wherever they appear regardless (see vfs.c's lookupNode/sfsLookup), so
   later relative resolution still behaves correctly either way; only
   vfsGetcwd's own output could then look non-canonical (e.g.
   "/mnt/foo/../bar") for an "/mnt" cwd reached that way -- an
   honestly-scoped simplification, not a fake canonicalization. */
int vfsChdir(const char* path);

/* Copies the calling thread's current working directory (always already
   absolute, see vfsChdir above) into `out`, nul-terminated, and returns its
   length -- or -1, writing nothing, if it does not fit `cap` (real
   getcwd(2)'s own ERANGE case). */
int64 vfsGetcwd(char* out, uint64 cap);

/* Resolves `path` against the CALLING thread's own cwd (sched.h's
   currentThreadCwd) into `out`: copied as-is (truncated to `cap`-1 if
   necessary) if it already starts with '/', otherwise joined onto the cwd
   exactly like a real relative open() would -- the same "cwd + '/' +
   argument" shape shell.c's own long-standing buildPath already used for
   its builtins, now the ONE place that logic lives. Every path-taking
   public entry point below (vfsOpen, vfsOpenInto, vfsMkdir, vfsUnlink,
   vfsRmdir, vfsStat, vfsSymlink's `path` argument -- never its `target`,
   which is stored verbatim like a real symlink(2) -- vfsReadlink, vfsLink,
   vfsMkfifo, vfsChmod) calls this internally before doing anything else, so an
   already-absolute path built by hand (shell.c's own callers, unchanged)
   keeps working exactly as before. Exposed so vfsChdir can resolve ITS OWN
   argument the same way before ever storing it, keeping Thread.cwd always
   fully absolute. */
void vfsResolvePath(const char* path, char* out, uint64 cap);

typedef struct VfsNode VfsNode;
VfsNode* vfsLookup(const char* path);
VfsNode* vfsChild(VfsNode* dir);
VfsNode* vfsNext(VfsNode* node);
const char* vfsName(VfsNode* node);
bool vfsIsDir(VfsNode* node);
uint64 vfsSize(VfsNode* node);
const void* vfsFileData(VfsNode* node);

/* Real owner rwx permission bits (fs.h's fsModeOwner*) for a tmpfs VfsNode
   -- the exact same value vfsStat's tmpfs branch reports as FbStat.mode.
   Exposed as its own accessor (VfsNode is opaque outside vfs.c, same reason
   vfsSize/vfsFileData exist) so jobctl.c's jobctlSpawn can check
   fsModeOwnerExec on an already-vfsLookup'd node before spawning it,
   without a redundant vfsStat + path re-resolve. */
uint32 vfsMode(VfsNode* node);

/* Real owner identity (fs.h's FbStat.uid) for a tmpfs VfsNode -- the uid
   that was currentThreadUid() at nodeAlloc time (vfs.c), mirroring
   vfsMode's own "opaque outside vfs.c" exposure exactly, for the same
   reason: syscall.c's loadImageByPath and jobctl.c's jobctlSpawn need this
   on an already-vfsLookup'd node before deciding whether to spawn it. */
uint32 vfsUid(VfsNode* node);

/* Real group identity (fs.h's FbStat.gid) for a tmpfs VfsNode -- the gid
   that was currentThreadGid() at nodeAlloc time, mirroring vfsUid's own
   "opaque outside vfs.c" exposure for the same reason (syscall.c's
   loadImageByPath, jobctl.c's jobctlSpawn). */
uint32 vfsGid(VfsNode* node);

/* Real Unix owner/group/other permission check (vfs.c): does the caller
   (`callerUid`/`callerGid`) have `wantOwnerBit` (one of fs.h's fsModeOwner*
   constants) against a node whose bits are `mode` and whose owner is
   `ownerUid`/`ownerGid`? A matching uid checks the owner bit; else a
   matching gid checks the corresponding "group" bit (fs.h's fsModeGroup*,
   wantOwnerBit >> 3); else the corresponding "other" bit (fs.h's
   fsModeOther*, wantOwnerBit >> 6) -- deliberately no uid-0-bypasses-
   everything superuser rule (see vfsPermitted's own doc comment in vfs.c
   for why). Exported (not just used internally by resolveOpen) so
   syscall.c's loadImageByPath and jobctl.c's jobctlSpawn can run the
   identical exec-bit check instead of each re-deriving this algorithm.
   Pure function of its six arguments -- callers pass currentThreadUid()/
   currentThreadGid() in explicitly, this never reads a live thread
   itself. */
bool vfsPermitted(uint32 mode, uint32 ownerUid, uint32 ownerGid, uint32 callerUid, uint32 callerGid, uint32 wantOwnerBit);
void vfsPath(VfsNode* node, char* out, uint64 cap);

#endif
