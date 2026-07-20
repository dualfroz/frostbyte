#include "vfs.h"
#include "initrd.h"
#include "heap.h"
#include "string.h"
#include "console.h"
#include "keyboard.h"
#include "sched.h"
#include "pipe.h"
#include "pidfd.h"
#include "ata.h"
#include "sfs.h"
#include "poll.h"

#define tmpfsBlock 512
#define blockDevPath "/dev/disk0"
#define sfsMountPrefix "/mnt"
#define sfsMountPrefixLen 4

struct VfsNode {
    char name[fsNameMax];
    uint32 type;
    bool writable;
    /* Real owner rwx permission bits (fs.h's fsModeOwner*), an axis
       independent of `writable` above: `writable` says whether this node's
       CONTENT/CHILDREN may be mutated at all (a mount-level property, false
       for every ustar-loaded node regardless of mode); `mode` is the real,
       chmod(2)-settable per-node permission a caller must ALSO satisfy to
       open the node for writing or (jobctl.c/syscall.c) exec it. Both gates
       must pass, exactly like a real read-only mount PLUS real per-file
       permission bits would. */
    uint32 mode;
    /* Real owner identity (POSIX uid, fs.h's FbStat.uid): whichever
       currentThreadUid() was current at nodeAlloc time -- always 0 ("root")
       for every ustar-loaded and boot-time tmpfs-mount node, since
       vfsMountUstar/vfsMountTmpfs both run after schedInit but before any
       real process exists (schedInit's idle thread, uid 0, is always
       "current" then). Never retroactive, like mode's own default; there is
       no chown-shaped call anywhere in this kernel to ever change it after
       creation (see fs.h's FbStat.uid doc comment). */
    uint32 uid;
    /* Real group identity (fs.h's FbStat.gid): whichever currentThreadGid()
       was current at nodeAlloc time, same creation-time-only, never-
       retroactive shape as uid immediately above. */
    uint32 gid;
    const uint8* roData;
    uint8* rwData;
    uint64 size;
    uint64 capacity;
    /* Only meaningful for type == fsTypeFifo: the pipe.h handle every
       vfsOpen on this path attaches a fresh end to (see makeChildFifo). -1
       for every other node type. */
    int32 pipeHandle;
    struct VfsNode* parent;
    struct VfsNode* firstChild;
    struct VfsNode* nextSibling;
};

static VfsNode* root;

static VfsNode* nodeAlloc(const char* name, uint64 nameLen, uint32 type, bool writable, uint32 mode)
{
    VfsNode* node = (VfsNode*)kmalloc(sizeof(VfsNode));
    if (!node)
        return nullptr;
    memset(node, 0, sizeof(VfsNode));
    if (nameLen >= fsNameMax)
        nameLen = fsNameMax - 1;
    memcpy(node->name, name, nameLen);
    node->name[nameLen] = 0;
    node->type = type;
    node->writable = writable;
    node->mode = mode;
    node->uid = currentThreadUid();
    node->gid = currentThreadGid();
    node->pipeHandle = -1;
    return node;
}

static void addChild(VfsNode* parent, VfsNode* child)
{
    child->parent = parent;
    child->nextSibling = parent->firstChild;
    parent->firstChild = child;
}

static VfsNode* findChild(VfsNode* parent, const char* name, uint64 nameLen)
{
    for (VfsNode* child = parent->firstChild; child; child = child->nextSibling)
        if (strlen(child->name) == nameLen && memcmp(child->name, name, nameLen) == 0)
            return child;
    return nullptr;
}

static VfsNode* makeChildDir(VfsNode* parent, const char* name, uint64 nameLen, bool writable, uint32 mode)
{
    VfsNode* node = nodeAlloc(name, nameLen, fsTypeDir, writable, mode);
    if (node)
        addChild(parent, node);
    return node;
}

static VfsNode* makeChildFile(VfsNode* parent, const char* name, uint64 nameLen,
                              const uint8* data, uint64 size, bool writable, uint32 mode)
{
    VfsNode* node = nodeAlloc(name, nameLen, fsTypeFile, writable, mode);
    if (!node)
        return nullptr;
    if (writable) {
        node->rwData = nullptr;
        node->capacity = 0;
        node->size = 0;
    } else {
        node->roData = data;
        node->size = size;
    }
    addChild(parent, node);
    return node;
}

static VfsNode* makeChildFifo(VfsNode* parent, const char* name, uint64 nameLen, int pipeHandle)
{
    VfsNode* node = nodeAlloc(name, nameLen, fsTypeFifo, true, fsModeDefaultFile);
    if (!node)
        return nullptr;
    node->pipeHandle = pipeHandle;
    addChild(parent, node);
    return node;
}

/* Real POSIX "search" permission: resolving a NAME inside a directory (as
   opposed to "." / ".." above, which never look anything up) requires the
   owner-or-other exec bit on that directory for the calling thread's own
   uid, exactly like a real kernel's namei() checks X_OK on every directory
   it walks through. Deliberately only gates the findChild() branch below,
   not "."/".." -- an honest, minor scope boundary matching this codebase's
   existing style of not fixing unrelated pre-existing gaps while adding a
   new feature (see this file's many other "deliberately out of scope"
   comments). Denial collapses to the same nullptr every other lookup
   failure already returns, matching every other EACCES-shaped refusal in
   this file (resolveOpen's write check, etc.) -- callers cannot tell
   "does not exist" from "exists but not searchable", exactly like the rest
   of this VFS layer's error handling. */
static VfsNode* lookupNode(const char* path)
{
    if (!path)
        return nullptr;

    VfsNode* cur = root;
    const char* p = path;
    while (*p == '/')
        p++;

    while (*p) {
        const char* start = p;
        while (*p && *p != '/')
            p++;
        uint64 len = (uint64)(p - start);

        if (len == 1 && start[0] == '.') {
            /* stay */
        } else if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (cur->parent)
                cur = cur->parent;
        } else {
            if (cur->type != fsTypeDir)
                return nullptr;
            if (!vfsPermitted(cur->mode, cur->uid, cur->gid, currentThreadUid(), currentThreadGid(), fsModeOwnerExec))
                return nullptr;
            VfsNode* child = findChild(cur, start, len);
            if (!child)
                return nullptr;
            cur = child;
        }

        while (*p == '/')
            p++;
    }
    return cur;
}

static VfsNode* splitParent(const char* path, char* leaf, uint64 leafCap)
{
    uint64 len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        len--;

    int64 slash = -1;
    for (int64 i = (int64)len - 1; i >= 0; i--)
        if (path[i] == '/') {
            slash = i;
            break;
        }

    const char* leafStart;
    uint64 leafLen;
    VfsNode* parent;

    if (slash < 0) {
        parent = root;
        leafStart = path;
        leafLen = len;
    } else {
        char parentBuf[256];
        uint64 plen = (slash == 0) ? 1 : (uint64)slash;
        if (plen > 255)
            plen = 255;
        memcpy(parentBuf, path, plen);
        parentBuf[plen] = 0;
        parent = lookupNode(parentBuf);
        leafStart = path + slash + 1;
        leafLen = len - (uint64)(slash + 1);
    }

    if (leafLen >= leafCap)
        leafLen = leafCap - 1;
    memcpy(leaf, leafStart, leafLen);
    leaf[leafLen] = 0;
    return parent;
}

static void ustarAdd(const char* rawPath, bool isDir, const uint8* data, uint64 size)
{
    char path[256];
    uint64 n = strlen(rawPath);
    while (n > 0 && rawPath[n - 1] == '/')
        n--;
    if (n == 0)
        return;
    if (n > 255)
        n = 255;
    memcpy(path, rawPath, n);
    path[n] = 0;

    VfsNode* cur = root;
    uint64 i = 0;
    while (i < n) {
        while (i < n && path[i] == '/')
            i++;
        uint64 start = i;
        while (i < n && path[i] != '/')
            i++;
        uint64 len = i - start;
        if (len == 0)
            break;

        bool last = true;
        for (uint64 j = i; j < n; j++)
            if (path[j] != '/') {
                last = false;
                break;
            }

        VfsNode* child = findChild(cur, path + start, len);
        if (last) {
            if (!child) {
                if (isDir)
                    child = makeChildDir(cur, path + start, len, false, fsModeDefaultDir);
                else {
                    /* Anything shipped under "bin/" (mktar's own stored-name
                       convention, no leading slash -- see tools/mktar.c) is a
                       real binary this kernel spawns by path (jobctlSpawn,
                       syscall.c's loadImageByPath): give it the owner-exec
                       bit by default, exactly as if someone had already run
                       `chmod 755` on it, since nothing in this kernel could
                       otherwise ever chmod a read-only ustar-loaded file
                       (vfsChmod refuses a non-writable node, same as every
                       other tmpfs mutation). Everything else defaults to
                       fsModeDefaultFile (no exec bit), matching a real
                       ordinary shipped data file. */
                    uint32 mode = (n > 4 && memcmp(path, "bin/", 4) == 0)
                                      ? fsModeDefaultExec
                                      : fsModeDefaultFile;
                    child = makeChildFile(cur, path + start, len, data, size, false, mode);
                }
            }
        } else if (!child) {
            child = makeChildDir(cur, path + start, len, false, fsModeDefaultDir);
        }
        if (!child)
            return;
        cur = child;
    }
}

void vfsInit(void)
{
    root = nodeAlloc("/", 1, fsTypeDir, false, fsModeDefaultDir);
}

void vfsMountUstar(uint64 start, uint64 end)
{
    initrdInit(start, end);
    uint32 count = initrdCount();
    uint32 files = 0;
    for (uint32 i = 0; i < count; i++) {
        UstarEntry entry;
        if (!initrdAt(i, &entry))
            break;
        ustarAdd(entry.name, entry.isDir, entry.data, entry.size);
        if (!entry.isDir)
            files++;
    }
    kprintf("[vfs]  mounted ustar at /, %u entries (%u files)\n", count, files);
}

void vfsMountTmpfs(const char* path)
{
    char leaf[fsNameMax];
    VfsNode* parent = splitParent(path, leaf, sizeof(leaf));
    if (!parent || parent->type != fsTypeDir)
        return;
    if (findChild(parent, leaf, strlen(leaf)))
        return;
    makeChildDir(parent, leaf, strlen(leaf), true, fsModeDefaultDir);
    kprintf("[vfs]  mounted writable tmpfs at %s\n", path);
}

void vfsFdInit(FileDescriptor* fds)
{
    for (uint32 i = 0; i < maxOpenFiles; i++) {
        fds[i].node = nullptr;
        fds[i].offset = 0;
        fds[i].kind = fdFree;
        fds[i].flags = 0;
        fds[i].pipeHandle = -1;
    }
    fds[0].kind = fdConsole;
    fds[1].kind = fdConsole;
    fds[2].kind = fdConsole;

    /* fd 0's own console reader queue (keyboard.h) -- reused field, same as
       a pipe/fifo fd's handle into pipe.c's table. Every thread (kernel and
       user alike) gets one, whether it ever reads stdin or not, matching
       fd 0 already always being fdConsole for every thread regardless of
       use. fdFlagBlocking makes syscall.c's sysRead retry-syscall path
       treat it exactly like a blocking pipe end (vfsBlockingChannel below);
       fd 1/2 (stdout/stderr) never read, so they get no reader and no
       blocking flag. If the reader table is exhausted, fd 0 is left
       non-blocking with an invalid handle -- reads on it then simply fail
       (-1) rather than crash, the same fail-open behavior a fully exhausted
       pipe/fifo table already has elsewhere in this file. */
    fds[0].pipeHandle = keyboardOpenReader();
    fds[0].flags = fdFlagBlocking;
}

static FileDescriptor* fdAt(int fd)
{
    if (fd < 0 || fd >= (int)maxOpenFiles)
        return nullptr;
    FileDescriptor* d = &currentThread()->fds[fd];
    if (d->kind == fdFree)
        return nullptr;
    return d;
}

static int allocFd(void)
{
    FileDescriptor* fds = currentThread()->fds;
    for (int i = 3; i < (int)maxOpenFiles; i++)
        if (fds[i].kind == fdFree)
            return i;
    return -1;
}

static bool ensureCapacity(VfsNode* node, uint64 needed)
{
    if (needed <= node->capacity)
        return true;
    uint64 newCap = (needed + (tmpfsBlock - 1)) & ~(uint64)(tmpfsBlock - 1);
    uint8* fresh = (uint8*)kmalloc(newCap);
    if (!fresh)
        return false;
    if (node->rwData) {
        memcpy(fresh, node->rwData, node->size);
        kfree(node->rwData);
    }
    node->rwData = fresh;
    node->capacity = newCap;
    return true;
}

static uint64 blockDevTotalBytes(void)
{
    return ataSectorCount() * (uint64)ataSectorSize;
}

/* Reads/writes at most one sector's worth per iteration so a request that
   straddles a sector boundary (or several) is handled as a sequence of
   aligned sector transfers, each preceded by a read-modify-write step when
   the access does not start or end on a sector boundary. */
static int64 blockDevRead(FileDescriptor* d, void* buffer, uint64 length)
{
    uint64 total = blockDevTotalBytes();
    if (d->offset >= total)
        return 0;
    uint64 want = length;
    if (want > total - d->offset)
        want = total - d->offset;

    uint8* dst = (uint8*)buffer;
    uint64 done = 0;
    while (done < want) {
        uint64 offset = d->offset + done;
        uint64 lba = offset / ataSectorSize;
        uint64 within = offset % ataSectorSize;
        uint64 chunk = ataSectorSize - within;
        if (chunk > want - done)
            chunk = want - done;

        uint8 sector[ataSectorSize];
        if (!ataReadSectors(lba, 1, sector))
            break;
        memcpy(dst + done, sector + within, chunk);
        done += chunk;
    }

    d->offset += done;
    if (done > 0)
        return (int64)done;
    return want == 0 ? 0 : -1;
}

static int64 blockDevWrite(FileDescriptor* d, const void* buffer, uint64 length)
{
    uint64 total = blockDevTotalBytes();
    if (d->offset >= total)
        return -1;
    uint64 want = length;
    if (want > total - d->offset)
        want = total - d->offset;

    const uint8* src = (const uint8*)buffer;
    uint64 done = 0;
    while (done < want) {
        uint64 offset = d->offset + done;
        uint64 lba = offset / ataSectorSize;
        uint64 within = offset % ataSectorSize;
        uint64 chunk = ataSectorSize - within;
        if (chunk > want - done)
            chunk = want - done;

        uint8 sector[ataSectorSize];
        if (chunk == ataSectorSize) {
            memcpy(sector, src + done, ataSectorSize);
        } else if (!ataReadSectors(lba, 1, sector)) {
            break;
        } else {
            memcpy(sector + within, src + done, chunk);
        }
        if (!ataWriteSectors(lba, 1, sector))
            break;
        done += chunk;
    }

    d->offset += done;
    if (done > 0)
        return (int64)done;
    return want == 0 ? 0 : -1;
}

/* True for "/mnt" itself and for anything under "/mnt/". relOut receives the
   path with the prefix stripped: "" (or "/foo") for sfsLookup, which treats
   an empty relative path as the sfs root. */
static bool isSfsPath(const char* path, const char** relOut)
{
    if (strncmp(path, sfsMountPrefix, sfsMountPrefixLen) != 0)
        return false;
    char after = path[sfsMountPrefixLen];
    if (after != '\0' && after != '/')
        return false;
    *relOut = path + sfsMountPrefixLen;
    return true;
}

/* See vfs.h's own doc comment for the full contract -- this is the single
   place shell.c's old per-builtin static cwd's logic now lives, shared by
   every path-taking entry point below instead of only shell.c's own
   handful of commands. */
void vfsResolvePath(const char* path, char* out, uint64 cap)
{
    if (path[0] == '/') {
        uint64 len = strlen(path);
        if (len >= cap)
            len = cap - 1;
        memcpy(out, path, len);
        out[len] = 0;
        return;
    }

    const char* cwd = currentThreadCwd();
    uint64 pos = strlen(cwd);
    if (pos >= cap)
        pos = cap - 1;
    memcpy(out, cwd, pos);
    if (pos == 0 || out[pos - 1] != '/') {
        if (pos < cap - 1)
            out[pos++] = '/';
    }
    uint64 i = 0;
    while (path[i] && pos < cap - 1)
        out[pos++] = path[i++];
    out[pos] = 0;
}

static uint32 sfsTypeToFsType(uint32 sfsType)
{
    if (sfsType == sfsTypeDir)
        return fsTypeDir;
    if (sfsType == sfsTypeFifo)
        return fsTypeFifo;
    return fsTypeFile;
}

/* Persistent (sfs.h) named-pipe bindings for "/mnt" fifos: sfs.c stores only
   that a path IS a fifo (the inode's type), never a live byte stream -- the
   actual pipe.h handle carrying bytes between two independent openers of
   the same persistent path is purely in-memory here, exactly like the
   VfsNode-resident one a tmpfs fifo carries on its own node forever (see
   makeChildFifo above), except an SFS inode number has nowhere on-disk to
   remember a handle. This small fixed table maps inode number -> live
   handle, created lazily on the FIRST open() of either direction (or the
   first open() since every previous session's ends have both gone away,
   see sfsFifoForget) and reused by every later open() of the same inode
   until then.
   Slot 0 is never a real fifo inode (allocInodeNum, sfs.c, starts at 1;
   inode 0 is permanently marked used by sfsFormat as a sentinel), so a
   zero-initialized table already starts every slot correctly "free" --
   inodeNum 0 doubles as "empty slot" the same way pipeHandle -1 already
   means "no handle" everywhere else in this file.
   Two INDEPENDENT staleness checks guard against two INDEPENDENT ways a
   remembered (inodeNum, handle) pair can stop meaning what it used to:
     - sfsGeneration() (sfs.h) catches "the whole filesystem underneath was
       wiped since" -- every sfsTest.c test starts with its own sfsFormat()
       call, which reuses inode numbers for completely unrelated entries
       dozens of times a boot; without this check, a binding left over from
       a wiped filesystem could silently hand a brand-new, unrelated
       inode's opener the OLD fifo's (possibly still-leaked) handle.
     - pipeIncarnation(handle) (pipe.h) catches "pipe.c's own table recycled
       this NUMBER for an unrelated pipe since" -- an entirely ordinary
       event that needs no filesystem format at all: the moment BOTH ends
       of a completed fifo round-trip close, pipeReleaseIfUnused frees the
       slot back to the general pool, and the very next unrelated
       vfsPipe()/vfsMkfifo() anywhere in the kernel is free to claim that
       exact number for something else entirely. pipeValid(handle) alone
       cannot tell "still my pipe, nobody has closed it" apart from
       "already reused by a stranger" -- only the incarnation counter can.
   Either mismatch is treated exactly like "never bound": the stale slot is
   evicted -- WITHOUT touching pipe.c, since whatever handle number it
   remembers may already mean something else by now -- and a fresh handle
   is created as if this were the first open ever. */
#define maxSfsFifoBindings 8

typedef struct SfsFifoBinding {
    int32 inodeNum;
    int32 pipeHandle;
    uint32 incarnation;
    uint64 generation;
} SfsFifoBinding;

static SfsFifoBinding sfsFifoBindings[maxSfsFifoBindings];

/* Finds inodeNum's live binding, discarding it first if it is stale by
   either check above. Returns nullptr if there is no valid binding (never
   bound, or just evicted as stale). */
static SfsFifoBinding* sfsFifoFind(int32 inodeNum)
{
    uint64 gen = sfsGeneration();
    for (uint32 i = 0; i < maxSfsFifoBindings; i++) {
        SfsFifoBinding* b = &sfsFifoBindings[i];
        if (b->inodeNum != inodeNum)
            continue;
        if (b->generation != gen || pipeIncarnation(b->pipeHandle) != b->incarnation) {
            b->inodeNum = 0;
            return nullptr;
        }
        return b;
    }
    return nullptr;
}

/* Returns inodeNum's live pipe handle, creating a fresh pipeCreateDetached
   one (and a new table slot) the first time this inode is opened since the
   last time nobody had it open -- exactly the "nobody attached yet, keep
   retrying rather than report broken/EOF" contract pipeCreateDetached
   itself already documents. -1 if pipe.c's own table is exhausted, or this
   inode has no existing binding and the (much smaller) binding table above
   is itself full. */
static int32 sfsFifoHandleFor(int32 inodeNum)
{
    SfsFifoBinding* existing = sfsFifoFind(inodeNum);
    if (existing)
        return existing->pipeHandle;

    int handle = pipeCreateDetached();
    if (handle < 0)
        return -1;

    for (uint32 i = 0; i < maxSfsFifoBindings; i++) {
        if (sfsFifoBindings[i].inodeNum == 0) {
            sfsFifoBindings[i].inodeNum = inodeNum;
            sfsFifoBindings[i].pipeHandle = handle;
            sfsFifoBindings[i].incarnation = pipeIncarnation(handle);
            sfsFifoBindings[i].generation = sfsGeneration();
            return handle;
        }
    }
    pipeDestroyIfUnattached(handle);
    return -1;
}

/* Called when inodeNum's own directory entry is actually gone for good
   (sfsRemove reached the last link, see vfsUnlink) -- evicts the table slot
   FIRST, then attempts pipeDestroyIfUnattached on the handle it held:
   unlinking a fifo that is currently open must leave the still-open ends
   alone (they keep working off their own copy of the handle number,
   matching the tmpfs fifo's own vfsUnlink comment below), so the destroy is
   a no-op unless nobody ever attached to it. Evicting before destroying
   (rather than after) closes the narrow window where a concurrent opener
   of the SAME inode number could otherwise see the stale slot as still
   valid and attach to a handle number pipe.c is about to hand out to
   something completely unrelated. */
static void sfsFifoForget(int32 inodeNum)
{
    for (uint32 i = 0; i < maxSfsFifoBindings; i++) {
        if (sfsFifoBindings[i].inodeNum == inodeNum) {
            int handle = sfsFifoBindings[i].pipeHandle;
            sfsFifoBindings[i].inodeNum = 0;
            pipeDestroyIfUnattached(handle);
            return;
        }
    }
}

static int64 sfsFdRead(FileDescriptor* d, void* buffer, uint64 length)
{
    int64 got = sfsReadAt(d->pipeHandle, d->offset, buffer, length);
    if (got > 0)
        d->offset += (uint64)got;
    return got;
}

static int64 sfsFdWrite(FileDescriptor* d, const void* buffer, uint64 length)
{
    int64 written = sfsWriteAt(d->pipeHandle, d->offset, buffer, length);
    if (written > 0)
        d->offset += (uint64)written;
    return written;
}

int vfsMkfifo(const char* path)
{
    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));
    path = resolvedPath;

    const char* rel;
    /* Persistent named pipe (sfs.h's sfsTypeFifo): only the directory entry
       and its type are created here -- the live pipe.h handle two openers
       actually rendezvous over is created lazily by vfsOpen the first time
       anyone opens it (sfsFifoHandleFor above), the same "create on first
       real use" split every other SFS-backed thing keeps between "what is
       on disk" (sfs.c, no pipe.h dependency at all) and "what is live in
       this boot's memory" (here). */
    if (isSfsPath(path, &rel))
        return sfsCreateFifo(rel) >= 0 ? 0 : -1;
    if (strcmp(path, blockDevPath) == 0)
        return -1;
    if (lookupNode(path))
        return -1;

    char leaf[fsNameMax];
    VfsNode* parent = splitParent(path, leaf, sizeof(leaf));
    if (!parent || parent->type != fsTypeDir || !parent->writable
            || !vfsPermitted(parent->mode, parent->uid, parent->gid, currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
        return -1;

    int handle = pipeCreateDetached();
    if (handle < 0)
        return -1;

    if (!makeChildFifo(parent, leaf, strlen(leaf), handle)) {
        pipeDestroyIfUnattached(handle);
        return -1;
    }
    return 0;
}

/* Shared core of vfsOpen/vfsOpenInto below: resolves `path`+`flags` and, on
   success, fills EVERY field of `d` (whichever FileDescriptor the caller
   decided on -- a freshly allocated slot in the calling thread's own table
   for vfsOpen, or an explicit slot in some other, not-yet-running thread's
   table for vfsOpenInto) and returns 0. On failure, returns -1 having
   written NOTHING to `d` at all -- every early return below is before the
   first store into `d`, so a caller never has to clean up a half-written
   descriptor. */
static int resolveOpen(FileDescriptor* d, const char* path, uint32 flags)
{
    const char* rel;
    if (isSfsPath(path, &rel)) {
        int32 inodeNum = sfsLookup(rel);
        if (inodeNum < 0) {
            if (!(flags & oCreate))
                return -1;
            inodeNum = sfsCreateFile(rel);
            if (inodeNum < 0)
                return -1;
        } else if (sfsTypeOf(inodeNum) == sfsTypeFifo) {
            /* One-directional only, exactly like the tmpfs fifo branch
               below and vfsPipeAttach's wantWrite bool -- a real O_RDWR
               fifo fd has no equivalent here, since a single fd can only be
               fdPipeRead or fdPipeWrite, never both. */
            uint32 mode = flags & (oWriteOnly | oReadWrite);
            bool wantWrite;
            if (mode == oWriteOnly)
                wantWrite = true;
            else if (mode == oReadOnly)
                wantWrite = false;
            else
                return -1;

            int handle = sfsFifoHandleFor(inodeNum);
            if (handle < 0)
                return -1;

            d->node = nullptr;
            d->offset = 0;
            d->kind = wantWrite ? fdPipeWrite : fdPipeRead;
            d->flags = fdFlagBlocking;
            d->pipeHandle = handle;

            if (wantWrite)
                pipeAddWriter(handle);
            else
                pipeAddReader(handle);
            return 0;
        } else {
            /* Real EACCES-shaped refusal: opening an EXISTING file for
               writing without the right write bit for the CALLING thread's
               identity (vfsPermitted, owner-vs-other-vs-root) fails at
               open() time, before any fd (and therefore any write()) ever
               exists -- matches real Unix, where permission is checked
               once, at open, never re-checked against an fd already held.
               A freshly oCreate'd file above always starts with
               fsModeDefaultFile owned by its creator (has the owner-write
               bit for exactly that uid), so this can never reject the
               create-a-new-file path. */
            if ((flags & (oWriteOnly | oReadWrite)) && sfsTypeOf(inodeNum) == sfsTypeFile
                    && !vfsPermitted(sfsModeOf(inodeNum), sfsUidOf(inodeNum), sfsGidOf(inodeNum),
                                     currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
                return -1;
            /* Real EACCES-shaped refusal, the read-side counterpart of the
               write check immediately above -- previously MISSING entirely
               (a genuine, previously-undiscovered gap this fala's own
               grouptest.c live demo found: a caller matching neither owner
               nor group could still read a file with no read bit for
               either, since nothing here ever consulted fsModeOwnerRead/
               fsModeGroupRead/fsModeOtherRead for a plain file open, only
               for a directory's own opendir()-shaped check below). Fires
               for oReadOnly AND oReadWrite (real Unix's O_RDWR needs read
               permission too, not just write) -- `!(flags & oWriteOnly)` is
               the same 2-bit-access-mode test the fifo branch above already
               uses to distinguish all three. */
            if (!(flags & oWriteOnly) && sfsTypeOf(inodeNum) == sfsTypeFile
                    && !vfsPermitted(sfsModeOf(inodeNum), sfsUidOf(inodeNum), sfsGidOf(inodeNum),
                                     currentThreadUid(), currentThreadGid(), fsModeOwnerRead))
                return -1;
            /* Real opendir()-shaped refusal: listing a directory's own
               entries (vfsGetdents below, on whatever fd this open()
               eventually returns) needs read permission on the directory
               ITSELF -- a separate bit from the exec/search permission
               lookupNode/lookupFrom already require on every ANCESTOR
               directory just to resolve this path down to here. */
            if (sfsTypeOf(inodeNum) == sfsTypeDir
                    && !vfsPermitted(sfsModeOf(inodeNum), sfsUidOf(inodeNum), sfsGidOf(inodeNum),
                                      currentThreadUid(), currentThreadGid(), fsModeOwnerRead))
                return -1;
            if ((flags & oTrunc) && sfsTypeOf(inodeNum) == sfsTypeFile)
                sfsTruncate(inodeNum, 0);
        }

        d->kind = fdSfs;
        d->node = nullptr;
        d->flags = flags;
        d->pipeHandle = inodeNum;
        d->offset = (flags & oAppend) ? sfsSizeOf(inodeNum) : 0;
        return 0;
    }

    if (strcmp(path, blockDevPath) == 0) {
        d->kind = fdBlockDev;
        d->node = nullptr;
        d->flags = flags;
        d->offset = 0;
        return 0;
    }

    VfsNode* node = lookupNode(path);
    if (!node) {
        if (!(flags & oCreate))
            return -1;
        char leaf[fsNameMax];
        VfsNode* parent = splitParent(path, leaf, sizeof(leaf));
        if (!parent || parent->type != fsTypeDir || !parent->writable
                || !vfsPermitted(parent->mode, parent->uid, parent->gid, currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
            return -1;
        node = makeChildFile(parent, leaf, strlen(leaf), nullptr, 0, true, fsModeDefaultFile);
        if (!node)
            return -1;
    } else if (node->type == fsTypeFifo) {
        /* One-directional only, exactly like vfsPipeAttach's wantWrite bool
           -- a real O_RDWR fifo fd (Linux allows it; POSIX leaves it
           undefined) has no equivalent here, since a single fd can only be
           fdPipeRead or fdPipeWrite, never both. */
        uint32 mode = flags & (oWriteOnly | oReadWrite);
        bool wantWrite;
        if (mode == oWriteOnly)
            wantWrite = true;
        else if (mode == oReadOnly)
            wantWrite = false;
        else
            return -1;

        d->node = nullptr;
        d->offset = 0;
        d->kind = wantWrite ? fdPipeWrite : fdPipeRead;
        d->flags = fdFlagBlocking;
        d->pipeHandle = node->pipeHandle;

        if (wantWrite)
            pipeAddWriter(node->pipeHandle);
        else
            pipeAddReader(node->pipeHandle);
        return 0;
    } else {
        /* Same real EACCES-shaped refusal as the SFS branch above, applied
           to tmpfs -- see that branch's comment for the full reasoning. */
        if ((flags & (oWriteOnly | oReadWrite)) && node->type == fsTypeFile
                && !vfsPermitted(node->mode, node->uid, node->gid, currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
            return -1;
        /* Same real file-read check as the SFS branch above (its own
           comment has the full "previously missing entirely" reasoning). */
        if (!(flags & oWriteOnly) && node->type == fsTypeFile
                && !vfsPermitted(node->mode, node->uid, node->gid, currentThreadUid(), currentThreadGid(), fsModeOwnerRead))
            return -1;
        /* Same opendir()-shaped read check as the SFS branch above. */
        if (node->type == fsTypeDir
                && !vfsPermitted(node->mode, node->uid, node->gid, currentThreadUid(), currentThreadGid(), fsModeOwnerRead))
            return -1;
        if ((flags & oTrunc) && node->type == fsTypeFile && node->writable)
            node->size = 0;
    }

    d->kind = fdFile;
    d->node = node;
    d->flags = flags;
    d->offset = (flags & oAppend) ? node->size : 0;
    return 0;
}

int vfsOpen(const char* path, uint32 flags)
{
    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));

    int fd = allocFd();
    if (fd < 0)
        return -1;
    if (resolveOpen(&currentThread()->fds[fd], resolvedPath, flags) < 0)
        return -1;
    return fd;
}

int vfsOpenInto(FileDescriptor* targetFds, int fd, const char* path, uint32 flags)
{
    /* Resolved against the CALLING thread's own cwd (the spawning parent,
       e.g. jobctl.c's redirect setup running before its not-yet-run child
       ever executes) -- exactly the cwd `targetFds`'s own owner already
       inherited a copy of at spawn time (initialCwd, sched.c), since
       nothing chdirs in between a spawn and its immediate redirect setup. */
    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));
    return resolveOpen(&targetFds[fd], resolvedPath, flags);
}

int vfsClose(int fd)
{
    FileDescriptor* d = fdAt(fd);
    if (!d)
        return -1;
    if (d->kind == fdConsole)
        return 0;
    if (d->kind == fdPipeRead)
        pipeCloseRead(d->pipeHandle);
    else if (d->kind == fdPipeWrite)
        pipeCloseWrite(d->pipeHandle);
    else if (d->kind == fdPidfd)
        pidfdClose(d->pipeHandle);
    d->kind = fdFree;
    d->node = nullptr;
    d->offset = 0;
    d->flags = 0;
    d->pipeHandle = -1;
    return 0;
}

int vfsPipe(int* readFd, int* writeFd)
{
    int rfd = allocFd();
    if (rfd < 0)
        return -1;
    currentThread()->fds[rfd].kind = fdPipeRead;

    int wfd = allocFd();
    if (wfd < 0) {
        currentThread()->fds[rfd].kind = fdFree;
        return -1;
    }
    currentThread()->fds[wfd].kind = fdPipeWrite;

    int handle = pipeCreate();
    if (handle < 0) {
        currentThread()->fds[rfd].kind = fdFree;
        currentThread()->fds[wfd].kind = fdFree;
        return -1;
    }

    currentThread()->fds[rfd].pipeHandle = handle;
    currentThread()->fds[wfd].pipeHandle = handle;
    *readFd = rfd;
    *writeFd = wfd;
    return 0;
}

int vfsPipeCreateHandle(void)
{
    return pipeCreateDetached();
}

int vfsPipeAttach(int handle, bool wantWrite)
{
    if (!pipeValid(handle))
        return -1;

    int fd = allocFd();
    if (fd < 0)
        return -1;

    FileDescriptor* d = &currentThread()->fds[fd];
    d->node = nullptr;
    d->offset = 0;
    d->kind = wantWrite ? fdPipeWrite : fdPipeRead;
    d->flags = fdFlagBlocking;
    d->pipeHandle = handle;

    if (wantWrite)
        pipeAddWriter(handle);
    else
        pipeAddReader(handle);
    return fd;
}

void vfsPipeAttachInto(FileDescriptor* targetFds, int fd, int handle, bool wantWrite)
{
    if (!pipeValid(handle))
        return;

    FileDescriptor* d = &targetFds[fd];
    d->node = nullptr;
    d->offset = 0;
    d->kind = wantWrite ? fdPipeWrite : fdPipeRead;
    d->flags = fdFlagBlocking;
    d->pipeHandle = handle;

    if (wantWrite)
        pipeAddWriter(handle);
    else
        pipeAddReader(handle);
}

int vfsPidfdOpen(uint32 targetId)
{
    int handle = pidfdOpen(targetId);
    if (handle < 0)
        return -1;

    int fd = allocFd();
    if (fd < 0) {
        pidfdClose(handle);
        return -1;
    }

    FileDescriptor* d = &currentThread()->fds[fd];
    d->node = nullptr;
    d->offset = 0;
    d->kind = fdPidfd;
    d->flags = 0;
    d->pipeHandle = handle;
    return fd;
}

uint32 vfsPidfdTarget(int fd)
{
    FileDescriptor* d = fdAt(fd);
    if (!d || d->kind != fdPidfd)
        return threadInvalidId;
    return pidfdTarget(d->pipeHandle);
}

const void* vfsBlockingChannel(int fd, bool forWrite)
{
    FileDescriptor* d = fdAt(fd);
    if (!d || !(d->flags & fdFlagBlocking))
        return nullptr;

    if (d->kind == fdPipeRead || d->kind == fdPipeWrite) {
        if (forWrite) {
            if (d->kind != fdPipeWrite || pipeWriteBroken(d->pipeHandle))
                return nullptr;
        } else if (d->kind != fdPipeRead) {
            return nullptr;
        }
        return pipeChannel(d->pipeHandle);
    }

    if (d->kind == fdConsole) {
        /* Console writes (kputchar) are never buffered and never block. */
        if (forWrite)
            return nullptr;
        return keyboardReaderChannel(d->pipeHandle);
    }

    return nullptr;
}

bool vfsPollReady(int fd, int16 events, int16* outRevents)
{
    int16 revents = 0;
    FileDescriptor* d = fdAt(fd);
    if (!d) {
        *outRevents = pollNval;
        return true;
    }

    switch (d->kind) {
    case fdPipeRead:
        if ((events & pollIn) && pipeHasData(d->pipeHandle))
            revents |= pollIn;
        break;
    case fdPipeWrite:
        if (pipeWriteBroken(d->pipeHandle))
            revents |= pollErr;
        else if ((events & pollOut) && pipeHasSpace(d->pipeHandle))
            revents |= pollOut;
        break;
    case fdConsole:
        if ((events & pollIn) && keyboardReaderHasData(d->pipeHandle))
            revents |= pollIn;
        /* Console writes (kputchar) are never buffered and never block --
           see vfsBlockingChannel above. */
        if (events & pollOut)
            revents |= pollOut;
        break;
    default:
        /* fdFile, fdBlockDev, fdSfs: every read/write on these runs to
           completion synchronously (see vfsRead/vfsWrite below), exactly
           like a real local filesystem always reports poll-ready. */
        revents |= (int16)(events & (pollIn | pollOut));
        break;
    }

    *outRevents = revents;
    return revents != 0;
}

int64 vfsWrite(int fd, const void* buffer, uint64 length)
{
    FileDescriptor* d = fdAt(fd);
    if (!d)
        return -1;

    if (d->kind == fdConsole) {
        const char* text = (const char*)buffer;
        for (uint64 i = 0; i < length; i++)
            kputchar(text[i]);
        return (int64)length;
    }

    if (d->kind == fdPipeWrite)
        return pipeWrite(d->pipeHandle, buffer, length);
    if (d->kind == fdPipeRead)
        return -1;
    if (d->kind == fdBlockDev)
        return blockDevWrite(d, buffer, length);
    if (d->kind == fdSfs)
        return sfsFdWrite(d, buffer, length);

    VfsNode* node = d->node;
    if (node->type != fsTypeFile || !node->writable)
        return -1;
    if (!ensureCapacity(node, d->offset + length))
        return -1;
    memcpy(node->rwData + d->offset, buffer, length);
    if (d->offset + length > node->size)
        node->size = d->offset + length;
    d->offset += length;
    return (int64)length;
}

int64 vfsRead(int fd, void* buffer, uint64 length)
{
    FileDescriptor* d = fdAt(fd);
    if (!d)
        return -1;

    if (d->kind == fdPipeRead)
        return pipeRead(d->pipeHandle, buffer, length);
    if (d->kind == fdPipeWrite)
        return -1;
    if (d->kind == fdBlockDev)
        return blockDevRead(d, buffer, length);
    if (d->kind == fdSfs)
        return sfsFdRead(d, buffer, length);

    if (d->kind == fdConsole) {
        /* Non-blocking, like every other real fd kind above: -1 (queue
           currently empty) is the "would block" signal syscall.c's sysRead
           retries via vfsBlockingChannel's channel + the restart-syscall
           trick, instead of this function looping/halting in place the way
           the old single-global-buffer keyboardReadChar() used to. Returns
           whatever is queued right now (possibly fewer than length bytes,
           same partial-read contract pipeRead already has) rather than
           insisting on a full line -- nothing in this kernel relied on the
           old line-buffering behavior (grep shows no caller ever read fd 0
           before this wave). */
        return keyboardReaderRead(d->pipeHandle, buffer, length);
    }

    VfsNode* node = d->node;
    if (node->type != fsTypeFile)
        return -1;
    const uint8* src = node->roData ? node->roData : node->rwData;
    if (d->offset >= node->size || !src)
        return 0;
    uint64 available = node->size - d->offset;
    uint64 take = length < available ? length : available;
    memcpy(buffer, src + d->offset, take);
    d->offset += take;
    return (int64)take;
}

int64 vfsLseek(int fd, int64 offset, int whence)
{
    FileDescriptor* d = fdAt(fd);
    if (!d || (d->kind != fdFile && d->kind != fdBlockDev && d->kind != fdSfs))
        return -1;

    int64 base;
    if (whence == seekSet)
        base = 0;
    else if (whence == seekCur)
        base = (int64)d->offset;
    else if (whence == seekEnd) {
        if (d->kind == fdBlockDev)
            base = (int64)blockDevTotalBytes();
        else if (d->kind == fdSfs)
            base = (int64)sfsSizeOf(d->pipeHandle);
        else
            base = (int64)d->node->size;
    } else
        return -1;

    int64 target = base + offset;
    if (target < 0)
        return -1;
    d->offset = (uint64)target;
    return target;
}

int vfsStat(const char* path, FbStat* out)
{
    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));
    path = resolvedPath;

    const char* rel;
    if (isSfsPath(path, &rel)) {
        int32 inodeNum = sfsLookup(rel);
        if (inodeNum < 0)
            return -1;
        out->size = sfsSizeOf(inodeNum);
        out->type = sfsTypeToFsType(sfsTypeOf(inodeNum));
        out->mode = sfsModeOf(inodeNum);
        out->uid = sfsUidOf(inodeNum);
        out->gid = sfsGidOf(inodeNum);
        out->mtime = sfsMtimeOf(inodeNum);
        out->ctime = sfsCtimeOf(inodeNum);
        return 0;
    }

    if (strcmp(path, blockDevPath) == 0) {
        out->size = blockDevTotalBytes();
        out->type = fsTypeFile;
        out->mode = 0666;
        out->uid = 0;
        out->gid = 0;
        out->mtime = 0;
        out->ctime = 0;
        return 0;
    }

    VfsNode* node = lookupNode(path);
    if (!node)
        return -1;
    out->size = node->size;
    out->type = node->type;
    out->mode = node->mode;
    out->uid = node->uid;
    out->gid = node->gid;
    out->mtime = 0;
    out->ctime = 0;
    return 0;
}

int64 vfsGetdents(int fd, void* buffer, uint64 length)
{
    FileDescriptor* d = fdAt(fd);
    if (!d || (d->kind != fdFile && d->kind != fdSfs))
        return -1;

    if (d->kind == fdSfs) {
        if (sfsTypeOf(d->pipeHandle) != sfsTypeDir)
            return -1;
        FbDirent* out = (FbDirent*)buffer;
        uint64 written = 0;
        char name[fsNameMax];
        uint32 type;
        while (written + sizeof(FbDirent) <= length &&
               sfsReaddirAt(d->pipeHandle, (uint32)d->offset, name, sizeof(name), &type)) {
            uint64 nameLen = strlen(name);
            out->type = sfsTypeToFsType(type);
            out->nameLength = (unsigned int)nameLen;
            memcpy(out->name, name, nameLen + 1);
            out++;
            written += sizeof(FbDirent);
            d->offset++;
        }
        return (int64)written;
    }

    VfsNode* node = d->node;
    if (node->type != fsTypeDir)
        return -1;

    FbDirent* out = (FbDirent*)buffer;
    uint64 written = 0;

    VfsNode* child = node->firstChild;
    for (uint64 skip = d->offset; child && skip > 0; skip--)
        child = child->nextSibling;

    while (child && written + sizeof(FbDirent) <= length) {
        uint64 nameLen = strlen(child->name);
        if (nameLen >= fsNameMax)
            nameLen = fsNameMax - 1;
        out->type = child->type;
        out->nameLength = (unsigned int)nameLen;
        memcpy(out->name, child->name, nameLen);
        out->name[nameLen] = 0;
        out++;
        written += sizeof(FbDirent);
        d->offset++;
        child = child->nextSibling;
    }
    return (int64)written;
}

int vfsMkdir(const char* path)
{
    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));
    path = resolvedPath;

    const char* rel;
    if (isSfsPath(path, &rel))
        return sfsCreateDir(rel) >= 0 ? 0 : -1;

    if (lookupNode(path))
        return -1;
    char leaf[fsNameMax];
    VfsNode* parent = splitParent(path, leaf, sizeof(leaf));
    if (!parent || parent->type != fsTypeDir || !parent->writable
            || !vfsPermitted(parent->mode, parent->uid, parent->gid, currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
        return -1;
    return makeChildDir(parent, leaf, strlen(leaf), true, fsModeDefaultDir) ? 0 : -1;
}

int vfsUnlink(const char* path)
{
    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));
    path = resolvedPath;

    const char* rel;
    if (isSfsPath(path, &rel)) {
        int32 inodeNum = sfsLookup(rel);
        /* Real unlink(2) semantics: EISDIR on a directory, even an empty
           one -- rmdir(2) (vfsRmdir below) is the only sanctioned way to
           remove one. sfsRemove itself has no opinion here (it is the raw
           on-disk primitive, also used by vfsRmdir below), so the type
           check has to happen at this dispatch layer, matching the tmpfs
           branch below which has always refused a directory this way. */
        if (inodeNum < 0 || sfsTypeOf(inodeNum) == sfsTypeDir)
            return -1;
        /* Only the LAST directory entry pointing at a persistent fifo's
           inode should ever tear down its live pipe.h binding (see
           sfsFifoForget above) -- a hardlinked fifo (sfsLink does not
           special-case sfsTypeFifo, unlike sfsTypeDir) losing one of
           several names must leave any live session alone, exactly like
           sfsRemove itself only actually frees the inode once linkCount
           drops to 0. Checked and captured BEFORE sfsRemove, since a
           successful removal may itself free the inode (readInode on it
           afterwards would read whatever unrelated data now occupies that
           slot, if anything). */
        bool lastLinkOfFifo = sfsTypeOf(inodeNum) == sfsTypeFifo &&
                               sfsLinkCountOf(inodeNum) <= 1;
        if (!sfsRemove(rel))
            return -1;
        if (lastLinkOfFifo)
            sfsFifoForget(inodeNum);
        return 0;
    }

    VfsNode* node = lookupNode(path);
    if (!node || !node->writable || (node->type != fsTypeFile && node->type != fsTypeFifo))
        return -1;

    /* Real unlink(2) semantics: removing a directory ENTRY needs write
       permission on the PARENT directory, not on the entry itself -- a
       real Unix caller can delete a read-only file it can write into the
       containing directory of, matching real Unix's own "the file's own
       mode is irrelevant to unlink" rule. */
    VfsNode* parent = node->parent;
    if (!vfsPermitted(parent->mode, parent->uid, parent->gid, currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
        return -1;

    VfsNode** link = &parent->firstChild;
    while (*link && *link != node)
        link = &(*link)->nextSibling;
    if (*link)
        *link = node->nextSibling;

    /* Any fd already opened on this fifo (fdPipeRead/fdPipeWrite) keeps
       working off its own copy of node->pipeHandle -- it never dereferences
       this node again, so freeing it here does not disturb a live reader or
       writer, matching real unlink-while-open semantics. Only reclaim the
       underlying pipe slot if nobody ever actually opened it; otherwise the
       still-open ends must keep it alive until their own vfsClose. */
    if (node->type == fsTypeFifo)
        pipeDestroyIfUnattached(node->pipeHandle);

    if (node->rwData)
        kfree(node->rwData);
    kfree(node);
    return 0;
}

VfsNode* vfsLookup(const char* path)
{
    return lookupNode(path);
}

VfsNode* vfsChild(VfsNode* dir)
{
    return dir ? dir->firstChild : nullptr;
}

VfsNode* vfsNext(VfsNode* node)
{
    return node ? node->nextSibling : nullptr;
}

const char* vfsName(VfsNode* node)
{
    return node->name;
}

bool vfsIsDir(VfsNode* node)
{
    return node->type == fsTypeDir;
}

uint64 vfsSize(VfsNode* node)
{
    return node->size;
}

const void* vfsFileData(VfsNode* node)
{
    if (!node)
        return nullptr;
    return node->roData ? (const void*)node->roData : (const void*)node->rwData;
}

void vfsPath(VfsNode* node, char* out, uint64 cap)
{
    if (cap < 2)
        return;
    if (node == nullptr || node == root) {
        out[0] = '/';
        out[1] = 0;
        return;
    }

    const char* parts[32];
    uint32 count = 0;
    for (VfsNode* n = node; n && n != root && count < 32; n = n->parent)
        parts[count++] = n->name;

    uint64 pos = 0;
    for (int32 i = (int32)count - 1; i >= 0; i--) {
        if (pos < cap - 1)
            out[pos++] = '/';
        const char* s = parts[i];
        for (uint32 j = 0; s[j] && pos < cap - 1; j++)
            out[pos++] = s[j];
    }
    if (pos == 0)
        out[pos++] = '/';
    out[pos] = 0;
}
