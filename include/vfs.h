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
    fdPidfd = 7
} FdKind;

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

bool vfsOwnerOrRoot(uint32 ownerUid, uint32 callerUid);

int vfsChmod(const char* path, uint32 mode);

int vfsChown(const char* path, uint32 newUid);

int vfsChgrp(const char* path, uint32 newGid);

int vfsSymlink(const char* path, const char* target);
int64 vfsReadlink(const char* path, char* buffer, uint64 cap);
int vfsLink(const char* existingPath, const char* newPath);

int vfsPipe(int* readFd, int* writeFd);

int vfsPipeCreateHandle(void);

int vfsPipeAttach(int handle, bool wantWrite);

void vfsPipeAttachInto(FileDescriptor* targetFds, int fd, int handle, bool wantWrite);

int vfsPidfdOpen(uint32 targetId);

uint32 vfsPidfdTarget(int fd);

int vfsOpenInto(FileDescriptor* targetFds, int fd, const char* path, uint32 flags);

const void* vfsBlockingChannel(int fd, bool forWrite);

bool vfsPollReady(int fd, int16 events, int16* outRevents);

int vfsMkfifo(const char* path);
const void* vfsFifoWriterWaitChannel(const char* path, uint32 flags);

int vfsChdir(const char* path);

int64 vfsGetcwd(char* out, uint64 cap);

void vfsResolvePath(const char* path, char* out, uint64 cap);

typedef struct VfsNode VfsNode;
VfsNode* vfsLookup(const char* path);
VfsNode* vfsChild(VfsNode* dir);
VfsNode* vfsNext(VfsNode* node);
const char* vfsName(VfsNode* node);
bool vfsIsDir(VfsNode* node);
uint64 vfsSize(VfsNode* node);
const void* vfsFileData(VfsNode* node);

uint32 vfsMode(VfsNode* node);

uint32 vfsUid(VfsNode* node);

uint32 vfsGid(VfsNode* node);

bool vfsPermitted(uint32 mode, uint32 ownerUid, uint32 ownerGid, uint32 callerUid, uint32 callerGid, uint32 wantOwnerBit);
void vfsPath(VfsNode* node, char* out, uint64 cap);

#endif
