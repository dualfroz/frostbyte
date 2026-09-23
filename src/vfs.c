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
    uint32 mode;
    uint32 uid;
    uint32 gid;
    const uint8* roData;
    uint8* rwData;
    uint64 size;
    uint64 capacity;
    int32 pipeHandle;
    uint32 fifoIncarnation;
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
    node->fifoIncarnation = pipeIncarnation(pipeHandle);
    addChild(parent, node);
    return node;
}

static int tmpfsFifoHandle(VfsNode* node)
{
    if (pipeIncarnation(node->pipeHandle) == node->fifoIncarnation)
        return node->pipeHandle;

    int handle = pipeCreateDetached();
    if (handle < 0)
        return -1;
    node->pipeHandle = handle;
    node->fifoIncarnation = pipeIncarnation(handle);
    return handle;
}

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

#define maxSfsFifoBindings 8

typedef struct SfsFifoBinding {
    int32 inodeNum;
    int32 pipeHandle;
    uint32 incarnation;
    uint64 generation;
} SfsFifoBinding;

static SfsFifoBinding sfsFifoBindings[maxSfsFifoBindings];

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

const void* vfsFifoWriterWaitChannel(const char* path, uint32 flags)
{
    if ((flags & (oWriteOnly | oReadWrite)) != oWriteOnly)
        return nullptr;

    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));

    int handle;
    const char* rel;
    if (isSfsPath(resolvedPath, &rel)) {
        int32 inodeNum = sfsLookup(rel);
        if (inodeNum < 0 || sfsTypeOf(inodeNum) != sfsTypeFifo)
            return nullptr;
        handle = sfsFifoHandleFor(inodeNum);
    } else {
        VfsNode* node = lookupNode(resolvedPath);
        if (!node || node->type != fsTypeFifo)
            return nullptr;
        handle = tmpfsFifoHandle(node);
    }

    if (handle < 0 || pipeHasReader(handle))
        return nullptr;
    return pipeChannel(handle);
}

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
            if ((flags & (oWriteOnly | oReadWrite)) && sfsTypeOf(inodeNum) == sfsTypeFile
                    && !vfsPermitted(sfsModeOf(inodeNum), sfsUidOf(inodeNum), sfsGidOf(inodeNum),
                                     currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
                return -1;
            if (!(flags & oWriteOnly) && sfsTypeOf(inodeNum) == sfsTypeFile
                    && !vfsPermitted(sfsModeOf(inodeNum), sfsUidOf(inodeNum), sfsGidOf(inodeNum),
                                     currentThreadUid(), currentThreadGid(), fsModeOwnerRead))
                return -1;
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
        uint32 mode = flags & (oWriteOnly | oReadWrite);
        bool wantWrite;
        if (mode == oWriteOnly)
            wantWrite = true;
        else if (mode == oReadOnly)
            wantWrite = false;
        else
            return -1;

        int handle = tmpfsFifoHandle(node);
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
        if ((flags & (oWriteOnly | oReadWrite)) && node->type == fsTypeFile
                && !vfsPermitted(node->mode, node->uid, node->gid, currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
            return -1;
        if (!(flags & oWriteOnly) && node->type == fsTypeFile
                && !vfsPermitted(node->mode, node->uid, node->gid, currentThreadUid(), currentThreadGid(), fsModeOwnerRead))
            return -1;
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
        if (events & pollOut)
            revents |= pollOut;
        break;
    default:
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
        if (inodeNum < 0 || sfsTypeOf(inodeNum) == sfsTypeDir)
            return -1;
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

    VfsNode* parent = node->parent;
    if (!vfsPermitted(parent->mode, parent->uid, parent->gid, currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
        return -1;

    VfsNode** link = &parent->firstChild;
    while (*link && *link != node)
        link = &(*link)->nextSibling;
    if (*link)
        *link = node->nextSibling;

    if (node->type == fsTypeFifo && pipeIncarnation(node->pipeHandle) == node->fifoIncarnation)
        pipeDestroyIfUnattached(node->pipeHandle);

    if (node->rwData)
        kfree(node->rwData);
    kfree(node);
    return 0;
}

int vfsRmdir(const char* path)
{
    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));
    path = resolvedPath;

    const char* rel;
    if (isSfsPath(path, &rel)) {
        int32 inodeNum = sfsLookup(rel);
        if (inodeNum < 0 || sfsTypeOf(inodeNum) != sfsTypeDir)
            return -1;
        return sfsRemove(rel) ? 0 : -1;
    }

    VfsNode* node = lookupNode(path);
    if (!node || !node->writable || node->type != fsTypeDir || node->firstChild)
        return -1;

    VfsNode* parent = node->parent;
    if (!vfsPermitted(parent->mode, parent->uid, parent->gid, currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
        return -1;

    VfsNode** link = &parent->firstChild;
    while (*link && *link != node)
        link = &(*link)->nextSibling;
    if (*link)
        *link = node->nextSibling;

    kfree(node);
    return 0;
}

bool vfsOwnerOrRoot(uint32 ownerUid, uint32 callerUid)
{
    return callerUid == 0 || callerUid == ownerUid;
}

int vfsChmod(const char* path, uint32 mode)
{
    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));
    path = resolvedPath;

    const char* rel;
    if (isSfsPath(path, &rel)) {
        int32 inodeNum = sfsLookup(rel);
        if (inodeNum < 0)
            return -1;
        if (!vfsOwnerOrRoot(sfsUidOf(inodeNum), currentThreadUid()))
            return -1;
        return sfsChmod(inodeNum, mode) ? 0 : -1;
    }

    VfsNode* node = lookupNode(path);
    if (!node || !node->writable)
        return -1;
    if (!vfsOwnerOrRoot(node->uid, currentThreadUid()))
        return -1;
    node->mode = mode;
    return 0;
}

int vfsChown(const char* path, uint32 newUid)
{
    if (currentThreadUid() != 0)
        return -1;

    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));
    path = resolvedPath;

    const char* rel;
    if (isSfsPath(path, &rel)) {
        int32 inodeNum = sfsLookup(rel);
        if (inodeNum < 0)
            return -1;
        return sfsChown(inodeNum, newUid) ? 0 : -1;
    }

    VfsNode* node = lookupNode(path);
    if (!node || !node->writable)
        return -1;
    node->uid = newUid;
    return 0;
}

int vfsChgrp(const char* path, uint32 newGid)
{
    uint32 callerUid = currentThreadUid();
    bool isRoot = callerUid == 0;
    if (!isRoot && newGid != currentThreadGid())
        return -1;

    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));
    path = resolvedPath;

    const char* rel;
    if (isSfsPath(path, &rel)) {
        int32 inodeNum = sfsLookup(rel);
        if (inodeNum < 0)
            return -1;
        if (!isRoot && sfsUidOf(inodeNum) != callerUid)
            return -1;
        return sfsChgrp(inodeNum, newGid) ? 0 : -1;
    }

    VfsNode* node = lookupNode(path);
    if (!node || !node->writable)
        return -1;
    if (!isRoot && node->uid != callerUid)
        return -1;
    node->gid = newGid;
    return 0;
}

int vfsSymlink(const char* path, const char* target)
{
    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));

    const char* rel;
    if (!isSfsPath(resolvedPath, &rel))
        return -1;
    return sfsCreateSymlink(rel, target) >= 0 ? 0 : -1;
}

int64 vfsReadlink(const char* path, char* buffer, uint64 cap)
{
    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));
    path = resolvedPath;

    const char* rel;
    if (!isSfsPath(path, &rel))
        return -1;

    int32 inodeNum = sfsLookupNoFollow(rel);
    if (inodeNum < 0 || sfsTypeOf(inodeNum) != sfsTypeSymlink)
        return -1;
    if (!sfsReadlink(inodeNum, buffer, cap))
        return -1;
    return (int64)strlen(buffer);
}

int vfsLink(const char* existingPath, const char* newPath)
{
    char resolvedExisting[fsPathMax];
    char resolvedNew[fsPathMax];
    vfsResolvePath(existingPath, resolvedExisting, sizeof(resolvedExisting));
    vfsResolvePath(newPath, resolvedNew, sizeof(resolvedNew));
    existingPath = resolvedExisting;
    newPath = resolvedNew;

    const char* existingRel;
    const char* newRel;
    if (!isSfsPath(existingPath, &existingRel) || !isSfsPath(newPath, &newRel))
        return -1;
    return sfsLink(existingRel, newRel) >= 0 ? 0 : -1;
}

int vfsChdir(const char* path)
{
    char resolvedPath[fsPathMax];
    vfsResolvePath(path, resolvedPath, sizeof(resolvedPath));

    const char* rel;
    if (isSfsPath(resolvedPath, &rel)) {
        int32 inodeNum = sfsLookup(rel);
        if (inodeNum < 0 || sfsTypeOf(inodeNum) != sfsTypeDir)
            return -1;
        return currentThreadSetCwd(resolvedPath) ? 0 : -1;
    }

    VfsNode* node = lookupNode(resolvedPath);
    if (!node || node->type != fsTypeDir)
        return -1;

    char canonical[fsPathMax];
    vfsPath(node, canonical, sizeof(canonical));
    return currentThreadSetCwd(canonical) ? 0 : -1;
}

int64 vfsGetcwd(char* out, uint64 cap)
{
    const char* cwd = currentThreadCwd();
    uint64 len = strlen(cwd);
    if (len >= cap)
        return -1;
    memcpy(out, cwd, len + 1);
    return (int64)len;
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

uint32 vfsMode(VfsNode* node)
{
    return node->mode;
}

uint32 vfsUid(VfsNode* node)
{
    return node->uid;
}

uint32 vfsGid(VfsNode* node)
{
    return node->gid;
}

bool vfsPermitted(uint32 mode, uint32 ownerUid, uint32 ownerGid, uint32 callerUid, uint32 callerGid, uint32 wantOwnerBit)
{
    if (callerUid == ownerUid)
        return (mode & wantOwnerBit) != 0;
    if (callerGid == ownerGid)
        return (mode & (wantOwnerBit >> 3)) != 0;
    return (mode & (wantOwnerBit >> 6)) != 0;
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
