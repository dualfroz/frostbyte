#ifndef FROSTBYTE_SFS_H
#define FROSTBYTE_SFS_H

#include "types.h"

#define sfsBlockSize   512
#define sfsBaseLba     100
#define sfsReserveEnd  16
#define sfsMaxNodes    512
#define sfsRootInode   1
#define sfsNameMax     55
#define sfsMaxSymlinkHops 8
#define sfsSymlinkTargetMax 255

#define sfsTypeFree    0
#define sfsTypeFile    1
#define sfsTypeDir     2
#define sfsTypeSymlink 3
#define sfsTypeFifo    4

bool sfsFormat(void);

bool sfsMount(void);

void sfsUnmount(void);

bool sfsMounted(void);

uint64 sfsCapacityBytes(void);

uint64 sfsFreeDataBlocks(void);

int32 sfsLookup(const char* path);

int32 sfsLookupNoFollow(const char* path);

int32 sfsCreateFile(const char* path);
int32 sfsCreateDir(const char* path);

int32 sfsCreateFifo(const char* path);

uint64 sfsGeneration(void);

int32 sfsCreateSymlink(const char* path, const char* target);

bool sfsReadlink(int32 inodeNum, char* buf, uint64 cap);

int32 sfsLink(const char* existingPath, const char* newPath);

uint32 sfsLinkCountOf(int32 inodeNum);

bool sfsRemove(const char* path);

uint64 sfsSizeOf(int32 inodeNum);
uint32 sfsTypeOf(int32 inodeNum);

uint64 sfsMtimeOf(int32 inodeNum);
uint64 sfsCtimeOf(int32 inodeNum);

uint32 sfsModeOf(int32 inodeNum);
bool sfsChmod(int32 inodeNum, uint32 mode);

uint32 sfsUidOf(int32 inodeNum);

bool sfsChown(int32 inodeNum, uint32 newUid);

uint32 sfsGidOf(int32 inodeNum);

bool sfsChgrp(int32 inodeNum, uint32 newGid);

bool sfsTruncate(int32 inodeNum, uint64 newSize);

int64 sfsReadAt(int32 inodeNum, uint64 offset, void* buffer, uint64 length);
int64 sfsWriteAt(int32 inodeNum, uint64 offset, const void* buffer, uint64 length);

bool sfsReaddirAt(int32 dirInode, uint32 index, char* nameOut, uint32 nameCap, uint32* typeOut);

#endif
