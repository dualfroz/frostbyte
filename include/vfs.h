#ifndef FROSTBYTE_VFS_H
#define FROSTBYTE_VFS_H

#include "types.h"
#include "fs.h"

struct VfsNode;

typedef enum {
    fdFree = 0,
    fdConsole = 1,
    fdFile = 2
} FdKind;

typedef struct FileDescriptor {
    struct VfsNode* node;
    uint64 offset;
    uint32 kind;
    uint32 flags;
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

typedef struct VfsNode VfsNode;
VfsNode* vfsLookup(const char* path);
VfsNode* vfsChild(VfsNode* dir);
VfsNode* vfsNext(VfsNode* node);
const char* vfsName(VfsNode* node);
bool vfsIsDir(VfsNode* node);
uint64 vfsSize(VfsNode* node);
const void* vfsFileData(VfsNode* node);
void vfsPath(VfsNode* node, char* out, uint64 cap);

#endif
