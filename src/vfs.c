#include "vfs.h"
#include "initrd.h"
#include "heap.h"
#include "string.h"
#include "console.h"
#include "keyboard.h"
#include "sched.h"

#define tmpfsBlock 512

struct VfsNode {
    char name[fsNameMax];
    uint32 type;
    bool writable;
    const uint8* roData;
    uint8* rwData;
    uint64 size;
    uint64 capacity;
    struct VfsNode* parent;
    struct VfsNode* firstChild;
    struct VfsNode* nextSibling;
};

static VfsNode* root;

static VfsNode* nodeAlloc(const char* name, uint64 nameLen, uint32 type, bool writable)
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

static VfsNode* makeChildDir(VfsNode* parent, const char* name, uint64 nameLen, bool writable)
{
    VfsNode* node = nodeAlloc(name, nameLen, fsTypeDir, writable);
    if (node)
        addChild(parent, node);
    return node;
}

static VfsNode* makeChildFile(VfsNode* parent, const char* name, uint64 nameLen,
                              const uint8* data, uint64 size, bool writable)
{
    VfsNode* node = nodeAlloc(name, nameLen, fsTypeFile, writable);
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
                    child = makeChildDir(cur, path + start, len, false);
                else
                    child = makeChildFile(cur, path + start, len, data, size, false);
            }
        } else if (!child) {
            child = makeChildDir(cur, path + start, len, false);
        }
        if (!child)
            return;
        cur = child;
    }
}

void vfsInit(void)
{
    root = nodeAlloc("/", 1, fsTypeDir, false);
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
    makeChildDir(parent, leaf, strlen(leaf), true);
    kprintf("[vfs]  mounted writable tmpfs at %s\n", path);
}

void vfsFdInit(FileDescriptor* fds)
{
    for (uint32 i = 0; i < maxOpenFiles; i++) {
        fds[i].node = nullptr;
        fds[i].offset = 0;
        fds[i].kind = fdFree;
        fds[i].flags = 0;
    }
    fds[0].kind = fdConsole;
    fds[1].kind = fdConsole;
    fds[2].kind = fdConsole;
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

int vfsOpen(const char* path, uint32 flags)
{
    VfsNode* node = lookupNode(path);
    if (!node) {
        if (!(flags & oCreate))
            return -1;
        char leaf[fsNameMax];
        VfsNode* parent = splitParent(path, leaf, sizeof(leaf));
        if (!parent || parent->type != fsTypeDir || !parent->writable)
            return -1;
        node = makeChildFile(parent, leaf, strlen(leaf), nullptr, 0, true);
        if (!node)
            return -1;
    } else if ((flags & oTrunc) && node->type == fsTypeFile && node->writable) {
        node->size = 0;
    }

    int fd = allocFd();
    if (fd < 0)
        return -1;

    FileDescriptor* d = &currentThread()->fds[fd];
    d->kind = fdFile;
    d->node = node;
    d->flags = flags;
    d->offset = (flags & oAppend) ? node->size : 0;
    return fd;
}

int vfsClose(int fd)
{
    FileDescriptor* d = fdAt(fd);
    if (!d)
        return -1;
    if (d->kind == fdConsole)
        return 0;
    d->kind = fdFree;
    d->node = nullptr;
    d->offset = 0;
    d->flags = 0;
    return 0;
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

    if (d->kind == fdConsole) {
        char* text = (char*)buffer;
        uint64 count = 0;
        while (count < length) {
            char c = keyboardReadChar();
            text[count++] = c;
            if (c == '\n')
                break;
        }
        return (int64)count;
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
    if (!d || d->kind != fdFile)
        return -1;

    int64 base;
    if (whence == seekSet)
        base = 0;
    else if (whence == seekCur)
        base = (int64)d->offset;
    else if (whence == seekEnd)
        base = (int64)d->node->size;
    else
        return -1;

    int64 target = base + offset;
    if (target < 0)
        return -1;
    d->offset = (uint64)target;
    return target;
}

int vfsStat(const char* path, FbStat* out)
{
    VfsNode* node = lookupNode(path);
    if (!node)
        return -1;
    out->size = node->size;
    out->type = node->type;
    out->mode = node->writable ? 0666 : 0444;
    return 0;
}

int64 vfsGetdents(int fd, void* buffer, uint64 length)
{
    FileDescriptor* d = fdAt(fd);
    if (!d || d->kind != fdFile)
        return -1;
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
    if (lookupNode(path))
        return -1;
    char leaf[fsNameMax];
    VfsNode* parent = splitParent(path, leaf, sizeof(leaf));
    if (!parent || parent->type != fsTypeDir || !parent->writable)
        return -1;
    return makeChildDir(parent, leaf, strlen(leaf), true) ? 0 : -1;
}

int vfsUnlink(const char* path)
{
    VfsNode* node = lookupNode(path);
    if (!node || node->type != fsTypeFile || !node->writable)
        return -1;

    VfsNode* parent = node->parent;
    VfsNode** link = &parent->firstChild;
    while (*link && *link != node)
        link = &(*link)->nextSibling;
    if (*link)
        *link = node->nextSibling;

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
