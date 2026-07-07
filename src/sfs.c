#include "sfs.h"
#include "ata.h"
#include "heap.h"
#include "string.h"
#include "rtc.h"
#include "fs.h"
#include "sched.h"
#include "vfs.h"

#define sfsDirectBlocks 10
#define sfsPtrsPerBlock (sfsBlockSize / sizeof(uint32)) /* 128 */

/* 88 bytes: 512/88 = 5 inodes per block (72 bytes/block left unused --
   inodeLocation and the format-time inodeTableBlocks math already derive
   everything from sizeof(SfsInode) generically, so this shrinking density
   is the honest cost of the extra fields, not a layout hack, same story as
   84 bytes/6-per-block was before gid). Addressing is direct (10 blocks) +
   single indirect (128 blocks) + double indirect (128 * 128 blocks) +
   triple indirect (128 * 128 * 128 blocks): a max file size of roughly
   (10 + 128 + 128*128 + 128*128*128) * 512 bytes ~= 1032 MiB, versus ~8 MiB
   with direct/single/double alone. mtime/ctime (rtc.h's rtcNow(), seconds
   since the Unix epoch, truncated to 32 bits -- valid through year 2106,
   same honest scope as rtc.c's own 2000-2099 century limit), mode (fs.h's
   fsModeOwner*, real chmod(2) bits), uid (fs.h's FbStat.uid, sched.h's
   currentThreadUid() at creation) and now gid (fs.h's FbStat.gid, sched.h's
   currentThreadGid() at creation) are the newest fields, genuinely growing
   the struct like tripleIndirect did before them -- there is no padding
   left to repurpose. */
typedef struct SfsInode {
    uint32 type;
    uint32 size;
    uint32 linkCount;
    uint32 blockCount;
    uint32 direct[sfsDirectBlocks];
    uint32 indirect;
    uint32 doubleIndirect;
    uint32 tripleIndirect;
    uint32 mtime;
    uint32 ctime;
    uint32 mode;
    uint32 uid;
    uint32 gid;
} SfsInode;

/* Exactly 64 bytes: 512/64 = 8 directory entries per block. name[56] leaves
   room for sfsNameMax (55) chars plus the null terminator. */
typedef struct SfsDirEntry {
    uint32 inode;
    uint32 type;
    char name[56];
} SfsDirEntry;

typedef struct SfsSuperblock {
    uint32 magic;
    uint32 blockSize;
    uint32 totalBlocks;
    uint32 inodeCount;
    uint32 inodeBitmapStart;
    uint32 inodeBitmapBlocks;
    uint32 blockBitmapStart;
    uint32 blockBitmapBlocks;
    uint32 inodeTableStart;
    uint32 inodeTableBlocks;
    uint32 dataStart;
    uint32 rootInode;
} SfsSuperblock;

#define sfsMagic 0x53465331u /* "SFS1" */

static SfsSuperblock sb;
static uint8* inodeBitmap = nullptr;
static uint8* blockBitmap = nullptr;
static bool isMounted = false;

/* See sfsGeneration()'s doc comment (sfs.h). Starts at 1 (not 0) so a caller
   that zero-initializes its own cache can use generation 0 to mean "never
   bound to anything yet" without that ever colliding with a real one. */
static uint64 formatGeneration = 1;

/* --- internal helpers (forward-declared so definition order can follow the
   dependency graph without header noise) --- */
static bool bitTest(const uint8* map, uint32 bit);
static void bitSet(uint8* map, uint32 bit);
static void bitClear(uint8* map, uint32 bit);
static bool blockRead(uint32 blockAbs, void* buffer);
static bool blockWrite(uint32 blockAbs, const void* buffer);
static bool flushInodeBitmap(void);
static bool flushBlockBitmap(void);
static bool flushSuperblock(void);
static void inodeLocation(int32 inodeNum, uint32* blockAbs, uint32* index);
static bool readInode(int32 inodeNum, SfsInode* out);
static bool writeInodeRaw(int32 inodeNum, const SfsInode* in);
static int32 allocInodeNum(void);
static void freeInodeNum(int32 inodeNum);
static int32 allocDataBlockAbs(void);
static void freeDataBlockAbs(uint32 blockAbs);
static bool loadOrAllocPointerBlock(SfsInode* inode, uint32* holderAbs, uint8* buf);
static bool getOrAllocBlockForWrite(SfsInode* inode, uint32 logicalIndex, uint32* outAbs);
static bool getBlockForRead(const SfsInode* inode, uint32 logicalIndex, uint32* outAbs);
static bool sfsWriteRaw(SfsInode* inode, int32 inodeNum, uint64 position, const void* data, uint64 length);
static bool sfsTruncateInode(SfsInode* inode, int32 inodeNum, uint32 newSize);
static bool splitPath(const char* path, char* parentOut, uint64 parentCap, char* leafOut, uint64 leafCap);
static int32 dirFind(int32 dirInode, const char* name, uint64 nameLen);
static bool dirAdd(int32 dirInode, const char* name, uint64 nameLen, int32 childInode, uint32 type);
static bool dirRemove(int32 dirInode, const char* name, uint64 nameLen);
static int32 createNode(const char* path, uint32 type);
static bool readSymlinkTarget(int32 inodeNum, char* buf, uint64 cap);
static int32 lookupFrom(const char* path, bool followFinal, int depth);

static bool bitTest(const uint8* map, uint32 bit)
{
    return (map[bit / 8] >> (bit % 8)) & 1u;
}

static void bitSet(uint8* map, uint32 bit)
{
    map[bit / 8] = (uint8)(map[bit / 8] | (uint8)(1u << (bit % 8)));
}

static void bitClear(uint8* map, uint32 bit)
{
    map[bit / 8] = (uint8)(map[bit / 8] & (uint8)~(1u << (bit % 8)));
}

static bool blockRead(uint32 blockAbs, void* buffer)
{
    return ataReadSectors(sfsBaseLba + blockAbs, 1, buffer);
}

static bool blockWrite(uint32 blockAbs, const void* buffer)
{
    return ataWriteSectors(sfsBaseLba + blockAbs, 1, buffer);
}

static bool flushInodeBitmap(void)
{
    for (uint32 i = 0; i < sb.inodeBitmapBlocks; i++)
        if (!blockWrite(sb.inodeBitmapStart + i, inodeBitmap + (uint64)i * sfsBlockSize))
            return false;
    return true;
}

static bool flushBlockBitmap(void)
{
    for (uint32 i = 0; i < sb.blockBitmapBlocks; i++)
        if (!blockWrite(sb.blockBitmapStart + i, blockBitmap + (uint64)i * sfsBlockSize))
            return false;
    return true;
}

static bool flushSuperblock(void)
{
    uint8 buf[sfsBlockSize];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &sb, sizeof(sb));
    return blockWrite(0, buf);
}

static void inodeLocation(int32 inodeNum, uint32* blockAbs, uint32* index)
{
    uint32 perBlock = sfsBlockSize / sizeof(SfsInode);
    *blockAbs = sb.inodeTableStart + (uint32)inodeNum / perBlock;
    *index = (uint32)inodeNum % perBlock;
}

/* Shared bound check for readInode/writeInodeRaw below: every REAL inode
   number allocInodeNum ever hands out satisfies 1 <= n < sb.inodeCount (the
   loop above never even considers slot 0, reserved as an eternal sentinel).
   Every one of this file's own internal callers already passes a value it
   got from allocInodeNum/sfsLookup/dirFind, so this never rejects a
   legitimate call -- it exists to stop an out-of-range inodeNum (a caller
   bug, or a deliberately-invalid probe like sfsModeOf(9999)) from silently
   computing a block address past the real inode table and reading/writing
   whatever data block happens to live there instead of failing cleanly. */
static bool inodeNumInRange(int32 inodeNum)
{
    return inodeNum > 0 && (uint32)inodeNum < sb.inodeCount;
}

static bool readInode(int32 inodeNum, SfsInode* out)
{
    if (!inodeNumInRange(inodeNum))
        return false;
    uint32 blk, idx;
    inodeLocation(inodeNum, &blk, &idx);
    uint8 buf[sfsBlockSize];
    if (!blockRead(blk, buf))
        return false;
    memcpy(out, buf + (uint64)idx * sizeof(SfsInode), sizeof(SfsInode));
    return true;
}

static bool writeInodeRaw(int32 inodeNum, const SfsInode* in)
{
    if (!inodeNumInRange(inodeNum))
        return false;
    uint32 blk, idx;
    inodeLocation(inodeNum, &blk, &idx);
    uint8 buf[sfsBlockSize];
    if (!blockRead(blk, buf))
        return false;
    memcpy(buf + (uint64)idx * sizeof(SfsInode), in, sizeof(SfsInode));
    return blockWrite(blk, buf);
}

static int32 allocInodeNum(void)
{
    for (uint32 i = 1; i < sb.inodeCount; i++) {
        if (!bitTest(inodeBitmap, i)) {
            bitSet(inodeBitmap, i);
            flushInodeBitmap();
            return (int32)i;
        }
    }
    return -1;
}

static void freeInodeNum(int32 inodeNum)
{
    bitClear(inodeBitmap, (uint32)inodeNum);
    flushInodeBitmap();
}

static int32 allocDataBlockAbs(void)
{
    uint32 dataBlockCount = sb.totalBlocks - sb.dataStart;
    for (uint32 i = 0; i < dataBlockCount; i++) {
        if (!bitTest(blockBitmap, i)) {
            bitSet(blockBitmap, i);
            flushBlockBitmap();
            uint32 abs = sb.dataStart + i;
            uint8 zero[sfsBlockSize];
            memset(zero, 0, sizeof(zero));
            blockWrite(abs, zero);
            return (int32)abs;
        }
    }
    return -1;
}

static void freeDataBlockAbs(uint32 blockAbs)
{
    bitClear(blockBitmap, blockAbs - sb.dataStart);
    flushBlockBitmap();
}

/* Allocates (if needed) blockAbs's holderAbs on first use and returns the
   holder block's contents in buf, zeroed for a fresh allocation. Shared by
   the single-indirect and each level of the double-indirect path below, so
   the "allocate a pointer block, zero it, write it back" dance is written
   once. *holderAbs is updated (and inode->blockCount bumped) when a new
   block is allocated. */
static bool loadOrAllocPointerBlock(SfsInode* inode, uint32* holderAbs, uint8* buf)
{
    if (*holderAbs == 0) {
        int32 nb = allocDataBlockAbs();
        if (nb < 0)
            return false;
        *holderAbs = (uint32)nb;
        inode->blockCount++;
        memset(buf, 0, sfsBlockSize);
        return blockWrite(*holderAbs, buf);
    }
    return blockRead(*holderAbs, buf);
}

static bool getOrAllocBlockForWrite(SfsInode* inode, uint32 logicalIndex, uint32* outAbs)
{
    if (logicalIndex < sfsDirectBlocks) {
        if (inode->direct[logicalIndex] == 0) {
            int32 nb = allocDataBlockAbs();
            if (nb < 0)
                return false;
            inode->direct[logicalIndex] = (uint32)nb;
            inode->blockCount++;
        }
        *outAbs = inode->direct[logicalIndex];
        return true;
    }

    uint32 indIndex = logicalIndex - sfsDirectBlocks;
    if (indIndex < sfsPtrsPerBlock) {
        uint8 indBuf[sfsBlockSize];
        if (!loadOrAllocPointerBlock(inode, &inode->indirect, indBuf))
            return false;

        uint32* ptrs = (uint32*)indBuf;
        if (ptrs[indIndex] == 0) {
            int32 nb = allocDataBlockAbs();
            if (nb < 0)
                return false;
            ptrs[indIndex] = (uint32)nb;
            inode->blockCount++;
            if (!blockWrite(inode->indirect, indBuf))
                return false;
        }
        *outAbs = ptrs[indIndex];
        return true;
    }

    uint32 doubleIndex = indIndex - sfsPtrsPerBlock;
    if (doubleIndex < sfsPtrsPerBlock * sfsPtrsPerBlock) {
        uint32 outerIndex = doubleIndex / sfsPtrsPerBlock;
        uint32 innerIndex = doubleIndex % sfsPtrsPerBlock;

        uint8 outerBuf[sfsBlockSize];
        if (!loadOrAllocPointerBlock(inode, &inode->doubleIndirect, outerBuf))
            return false;

        uint32* outerPtrs = (uint32*)outerBuf;
        uint8 innerBuf[sfsBlockSize];
        bool outerNeedsFlush = false;
        if (outerPtrs[outerIndex] == 0) {
            int32 nb = allocDataBlockAbs();
            if (nb < 0)
                return false;
            outerPtrs[outerIndex] = (uint32)nb;
            inode->blockCount++;
            outerNeedsFlush = true;
            memset(innerBuf, 0, sizeof(innerBuf));
            if (!blockWrite(outerPtrs[outerIndex], innerBuf))
                return false;
        } else if (!blockRead(outerPtrs[outerIndex], innerBuf)) {
            return false;
        }
        if (outerNeedsFlush && !blockWrite(inode->doubleIndirect, outerBuf))
            return false;

        uint32* innerPtrs = (uint32*)innerBuf;
        if (innerPtrs[innerIndex] == 0) {
            int32 nb = allocDataBlockAbs();
            if (nb < 0)
                return false;
            innerPtrs[innerIndex] = (uint32)nb;
            inode->blockCount++;
            if (!blockWrite(outerPtrs[outerIndex], innerBuf))
                return false;
        }
        *outAbs = innerPtrs[innerIndex];
        return true;
    }

    /* Same three-level shape as the double-indirect block above, one level
       deeper: tripleIndirect -> top (128 mid-block pointers) -> mid (128
       leaf-block pointers) -> leaf (128 data-block pointers) -> data. */
    uint32 tripleIndex = doubleIndex - sfsPtrsPerBlock * sfsPtrsPerBlock;
    uint32 topIndex = tripleIndex / (sfsPtrsPerBlock * sfsPtrsPerBlock);
    uint32 midIndex = (tripleIndex / sfsPtrsPerBlock) % sfsPtrsPerBlock;
    uint32 leafIndex = tripleIndex % sfsPtrsPerBlock;
    if (topIndex >= sfsPtrsPerBlock)
        return false; /* past the addressing limit (~1 GiB) */

    uint8 topBuf[sfsBlockSize];
    if (!loadOrAllocPointerBlock(inode, &inode->tripleIndirect, topBuf))
        return false;

    uint32* topPtrs = (uint32*)topBuf;
    uint8 midBuf[sfsBlockSize];
    bool topNeedsFlush = false;
    if (topPtrs[topIndex] == 0) {
        int32 nb = allocDataBlockAbs();
        if (nb < 0)
            return false;
        topPtrs[topIndex] = (uint32)nb;
        inode->blockCount++;
        topNeedsFlush = true;
        memset(midBuf, 0, sizeof(midBuf));
        if (!blockWrite(topPtrs[topIndex], midBuf))
            return false;
    } else if (!blockRead(topPtrs[topIndex], midBuf)) {
        return false;
    }
    if (topNeedsFlush && !blockWrite(inode->tripleIndirect, topBuf))
        return false;

    uint32* midPtrs = (uint32*)midBuf;
    uint8 leafBuf[sfsBlockSize];
    bool midNeedsFlush = false;
    if (midPtrs[midIndex] == 0) {
        int32 nb = allocDataBlockAbs();
        if (nb < 0)
            return false;
        midPtrs[midIndex] = (uint32)nb;
        inode->blockCount++;
        midNeedsFlush = true;
        memset(leafBuf, 0, sizeof(leafBuf));
        if (!blockWrite(midPtrs[midIndex], leafBuf))
            return false;
    } else if (!blockRead(midPtrs[midIndex], leafBuf)) {
        return false;
    }
    if (midNeedsFlush && !blockWrite(topPtrs[topIndex], midBuf))
        return false;

    uint32* leafPtrs = (uint32*)leafBuf;
    if (leafPtrs[leafIndex] == 0) {
        int32 nb = allocDataBlockAbs();
        if (nb < 0)
            return false;
        leafPtrs[leafIndex] = (uint32)nb;
        inode->blockCount++;
        if (!blockWrite(midPtrs[midIndex], leafBuf))
            return false;
    }
    *outAbs = leafPtrs[leafIndex];
    return true;
}

static bool getBlockForRead(const SfsInode* inode, uint32 logicalIndex, uint32* outAbs)
{
    if (logicalIndex < sfsDirectBlocks) {
        *outAbs = inode->direct[logicalIndex];
        return *outAbs != 0;
    }

    uint32 indIndex = logicalIndex - sfsDirectBlocks;
    if (indIndex < sfsPtrsPerBlock) {
        if (inode->indirect == 0)
            return false;
        uint8 indBuf[sfsBlockSize];
        if (!blockRead(inode->indirect, indBuf))
            return false;
        uint32* ptrs = (uint32*)indBuf;
        *outAbs = ptrs[indIndex];
        return *outAbs != 0;
    }

    uint32 doubleIndex = indIndex - sfsPtrsPerBlock;
    if (doubleIndex < sfsPtrsPerBlock * sfsPtrsPerBlock) {
        uint32 outerIndex = doubleIndex / sfsPtrsPerBlock;
        uint32 innerIndex = doubleIndex % sfsPtrsPerBlock;
        if (inode->doubleIndirect == 0)
            return false;

        uint8 outerBuf[sfsBlockSize];
        if (!blockRead(inode->doubleIndirect, outerBuf))
            return false;
        uint32* outerPtrs = (uint32*)outerBuf;
        if (outerPtrs[outerIndex] == 0)
            return false;

        uint8 innerBuf[sfsBlockSize];
        if (!blockRead(outerPtrs[outerIndex], innerBuf))
            return false;
        uint32* innerPtrs = (uint32*)innerBuf;
        *outAbs = innerPtrs[innerIndex];
        return *outAbs != 0;
    }

    uint32 tripleIndex = doubleIndex - sfsPtrsPerBlock * sfsPtrsPerBlock;
    uint32 topIndex = tripleIndex / (sfsPtrsPerBlock * sfsPtrsPerBlock);
    uint32 midIndex = (tripleIndex / sfsPtrsPerBlock) % sfsPtrsPerBlock;
    uint32 leafIndex = tripleIndex % sfsPtrsPerBlock;
    if (topIndex >= sfsPtrsPerBlock || inode->tripleIndirect == 0)
        return false;

    uint8 topBuf[sfsBlockSize];
    if (!blockRead(inode->tripleIndirect, topBuf))
        return false;
    uint32* topPtrs = (uint32*)topBuf;
    if (topPtrs[topIndex] == 0)
        return false;

    uint8 midBuf[sfsBlockSize];
    if (!blockRead(topPtrs[topIndex], midBuf))
        return false;
    uint32* midPtrs = (uint32*)midBuf;
    if (midPtrs[midIndex] == 0)
        return false;

    uint8 leafBuf[sfsBlockSize];
    if (!blockRead(midPtrs[midIndex], leafBuf))
        return false;
    uint32* leafPtrs = (uint32*)leafBuf;
    *outAbs = leafPtrs[leafIndex];
    return *outAbs != 0;
}

static bool sfsWriteRaw(SfsInode* inode, int32 inodeNum, uint64 position, const void* data, uint64 length)
{
    if (length == 0)
        return true;

    const uint8* src = (const uint8*)data;
    uint64 done = 0;
    while (done < length) {
        uint64 pos = position + done;
        uint32 logicalBlock = (uint32)(pos / sfsBlockSize);
        uint32 within = (uint32)(pos % sfsBlockSize);
        uint32 chunk = sfsBlockSize - within;
        if ((uint64)chunk > length - done)
            chunk = (uint32)(length - done);

        uint32 blockAbs;
        if (!getOrAllocBlockForWrite(inode, logicalBlock, &blockAbs))
            return false;

        uint8 blockBuf[sfsBlockSize];
        if (chunk == sfsBlockSize) {
            if (src)
                memcpy(blockBuf, src + done, sfsBlockSize);
            else
                memset(blockBuf, 0, sfsBlockSize);
        } else {
            if (!blockRead(blockAbs, blockBuf))
                return false;
            if (src)
                memcpy(blockBuf + within, src + done, chunk);
            else
                memset(blockBuf + within, 0, chunk);
        }
        if (!blockWrite(blockAbs, blockBuf))
            return false;

        done += chunk;
    }

    uint64 newSize = position + length;
    if (newSize > inode->size)
        inode->size = (uint32)newSize;
    inode->mtime = inode->ctime = (uint32)rtcNow();
    return writeInodeRaw(inodeNum, inode);
}

static bool sfsTruncateInode(SfsInode* inode, int32 inodeNum, uint32 newSize)
{
    uint32 oldBlocks = inode->size ? (inode->size + sfsBlockSize - 1) / sfsBlockSize : 0;
    uint32 neededBlocks = newSize ? (newSize + sfsBlockSize - 1) / sfsBlockSize : 0;

    for (uint32 i = neededBlocks; i < oldBlocks; i++) {
        uint32 abs = 0;
        if (i < sfsDirectBlocks) {
            abs = inode->direct[i];
            inode->direct[i] = 0;
        } else {
            uint32 indIndex = i - sfsDirectBlocks;
            if (indIndex < sfsPtrsPerBlock) {
                if (inode->indirect == 0)
                    continue;
                uint8 indBuf[sfsBlockSize];
                if (!blockRead(inode->indirect, indBuf))
                    continue;
                uint32* ptrs = (uint32*)indBuf;
                abs = ptrs[indIndex];
                ptrs[indIndex] = 0;
                blockWrite(inode->indirect, indBuf);
            } else {
                uint32 doubleIndex = indIndex - sfsPtrsPerBlock;
                if (doubleIndex < sfsPtrsPerBlock * sfsPtrsPerBlock) {
                    uint32 outerIndex = doubleIndex / sfsPtrsPerBlock;
                    uint32 innerIndex = doubleIndex % sfsPtrsPerBlock;
                    if (inode->doubleIndirect == 0)
                        continue;
                    uint8 outerBuf[sfsBlockSize];
                    if (!blockRead(inode->doubleIndirect, outerBuf))
                        continue;
                    uint32* outerPtrs = (uint32*)outerBuf;
                    if (outerPtrs[outerIndex] == 0)
                        continue;
                    uint8 innerBuf[sfsBlockSize];
                    if (!blockRead(outerPtrs[outerIndex], innerBuf))
                        continue;
                    uint32* innerPtrs = (uint32*)innerBuf;
                    abs = innerPtrs[innerIndex];
                    innerPtrs[innerIndex] = 0;
                    blockWrite(outerPtrs[outerIndex], innerBuf);
                } else {
                    uint32 tripleIndex = doubleIndex - sfsPtrsPerBlock * sfsPtrsPerBlock;
                    uint32 topIndex = tripleIndex / (sfsPtrsPerBlock * sfsPtrsPerBlock);
                    uint32 midIndex = (tripleIndex / sfsPtrsPerBlock) % sfsPtrsPerBlock;
                    uint32 leafIndex = tripleIndex % sfsPtrsPerBlock;
                    if (inode->tripleIndirect == 0 || topIndex >= sfsPtrsPerBlock)
                        continue;
                    uint8 topBuf[sfsBlockSize];
                    if (!blockRead(inode->tripleIndirect, topBuf))
                        continue;
                    uint32* topPtrs = (uint32*)topBuf;
                    if (topPtrs[topIndex] == 0)
                        continue;
                    uint8 midBuf[sfsBlockSize];
                    if (!blockRead(topPtrs[topIndex], midBuf))
                        continue;
                    uint32* midPtrs = (uint32*)midBuf;
                    if (midPtrs[midIndex] == 0)
                        continue;
                    uint8 leafBuf[sfsBlockSize];
                    if (!blockRead(midPtrs[midIndex], leafBuf))
                        continue;
                    uint32* leafPtrs = (uint32*)leafBuf;
                    abs = leafPtrs[leafIndex];
                    leafPtrs[leafIndex] = 0;
                    blockWrite(midPtrs[midIndex], leafBuf);
                }
            }
        }
        if (abs != 0) {
            freeDataBlockAbs(abs);
            inode->blockCount--;
        }
    }

    /* Pointer-holder blocks themselves (the indirect block, every inner
       indirect block plus the outer double-indirect block, and every
       top/mid/leaf-pointer block of the triple-indirect chain) are
       reclaimed all-or-nothing once nothing addressed through them is
       needed anymore -- the loop above already frees every leaf data block
       that was in range, so by this point each holder either still guards
       live data (neededBlocks reaches into its range: leave it alone) or is
       entirely spent (free it, and everything it pointed at). Checked
       highest level first (triple, then double, then single) so a shrink
       that drops the whole file frees every level in one pass. */
    if (neededBlocks <= sfsDirectBlocks + sfsPtrsPerBlock + sfsPtrsPerBlock * sfsPtrsPerBlock
            && inode->tripleIndirect != 0) {
        uint8 topBuf[sfsBlockSize];
        if (blockRead(inode->tripleIndirect, topBuf)) {
            uint32* topPtrs = (uint32*)topBuf;
            for (uint32 j = 0; j < sfsPtrsPerBlock; j++) {
                if (topPtrs[j] == 0)
                    continue;
                uint8 midBuf[sfsBlockSize];
                if (blockRead(topPtrs[j], midBuf)) {
                    uint32* midPtrs = (uint32*)midBuf;
                    for (uint32 k = 0; k < sfsPtrsPerBlock; k++) {
                        if (midPtrs[k] != 0) {
                            freeDataBlockAbs(midPtrs[k]);
                            inode->blockCount--;
                        }
                    }
                }
                freeDataBlockAbs(topPtrs[j]);
                inode->blockCount--;
            }
        }
        freeDataBlockAbs(inode->tripleIndirect);
        inode->tripleIndirect = 0;
        inode->blockCount--;
    }

    if (neededBlocks <= sfsDirectBlocks + sfsPtrsPerBlock && inode->doubleIndirect != 0) {
        uint8 outerBuf[sfsBlockSize];
        if (blockRead(inode->doubleIndirect, outerBuf)) {
            uint32* outerPtrs = (uint32*)outerBuf;
            for (uint32 j = 0; j < sfsPtrsPerBlock; j++) {
                if (outerPtrs[j] != 0) {
                    freeDataBlockAbs(outerPtrs[j]);
                    inode->blockCount--;
                }
            }
        }
        freeDataBlockAbs(inode->doubleIndirect);
        inode->doubleIndirect = 0;
        inode->blockCount--;
    }

    if (neededBlocks <= sfsDirectBlocks && inode->indirect != 0) {
        freeDataBlockAbs(inode->indirect);
        inode->indirect = 0;
        inode->blockCount--;
    }

    inode->size = newSize;
    inode->mtime = inode->ctime = (uint32)rtcNow();
    return writeInodeRaw(inodeNum, inode);
}

static bool splitPath(const char* path, char* parentOut, uint64 parentCap, char* leafOut, uint64 leafCap)
{
    uint64 len = strlen(path);
    while (len > 0 && path[len - 1] == '/')
        len--;
    if (len == 0)
        return false;

    int64 slash = -1;
    for (int64 i = (int64)len - 1; i >= 0; i--)
        if (path[i] == '/') {
            slash = i;
            break;
        }

    uint64 leafStart, leafLen;
    if (slash < 0) {
        parentOut[0] = 0;
        leafStart = 0;
        leafLen = len;
    } else {
        uint64 plen = (uint64)slash;
        if (plen >= parentCap)
            plen = parentCap - 1;
        memcpy(parentOut, path, plen);
        parentOut[plen] = 0;
        leafStart = (uint64)slash + 1;
        leafLen = len - leafStart;
    }

    if (leafLen == 0 || leafLen >= leafCap)
        return false;
    memcpy(leafOut, path + leafStart, leafLen);
    leafOut[leafLen] = 0;
    return true;
}

static int32 dirFind(int32 dirInode, const char* name, uint64 nameLen)
{
    SfsDirEntry entry;
    uint64 pos = 0;
    for (;;) {
        int64 got = sfsReadAt(dirInode, pos, &entry, sizeof(entry));
        if (got != (int64)sizeof(entry))
            break;
        if (strlen(entry.name) == nameLen && memcmp(entry.name, name, nameLen) == 0)
            return (int32)entry.inode;
        pos += sizeof(entry);
    }
    return -1;
}

static bool dirAdd(int32 dirInode, const char* name, uint64 nameLen, int32 childInode, uint32 type)
{
    if (nameLen == 0 || nameLen >= sfsNameMax)
        return false;
    if (dirFind(dirInode, name, nameLen) >= 0)
        return false;

    SfsDirEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.inode = (uint32)childInode;
    entry.type = type;
    memcpy(entry.name, name, nameLen);
    entry.name[nameLen] = 0;

    uint64 size = sfsSizeOf(dirInode);
    return sfsWriteAt(dirInode, size, &entry, sizeof(entry)) == (int64)sizeof(entry);
}

static bool dirRemove(int32 dirInode, const char* name, uint64 nameLen)
{
    SfsDirEntry entry;
    uint64 pos = 0;
    int64 foundPos = -1;
    for (;;) {
        int64 got = sfsReadAt(dirInode, pos, &entry, sizeof(entry));
        if (got != (int64)sizeof(entry))
            break;
        if (strlen(entry.name) == nameLen && memcmp(entry.name, name, nameLen) == 0) {
            foundPos = (int64)pos;
            break;
        }
        pos += sizeof(entry);
    }
    if (foundPos < 0)
        return false;

    uint64 size = sfsSizeOf(dirInode);
    uint64 lastPos = size - sizeof(SfsDirEntry);
    if ((uint64)foundPos != lastPos) {
        SfsDirEntry last;
        if (sfsReadAt(dirInode, lastPos, &last, sizeof(last)) != (int64)sizeof(last))
            return false;
        if (sfsWriteAt(dirInode, (uint64)foundPos, &last, sizeof(last)) != (int64)sizeof(last))
            return false;
    }

    SfsInode inode;
    if (!readInode(dirInode, &inode))
        return false;
    return sfsTruncateInode(&inode, dirInode, (uint32)lastPos);
}

static int32 createNode(const char* path, uint32 type)
{
    if (!isMounted)
        return -1;

    char parentBuf[256];
    char leaf[sfsNameMax];
    if (!splitPath(path, parentBuf, sizeof(parentBuf), leaf, sizeof(leaf)))
        return -1;

    int32 parentInode = sfsLookup(parentBuf);
    if (parentInode < 0 || sfsTypeOf(parentInode) != sfsTypeDir)
        return -1;
    /* Real Unix directory-write semantics: creating a NEW entry needs
       write permission on the PARENT directory, mirroring vfs.c's
       vfsMkdir/resolveOpen create-branch's own equivalent tmpfs check. */
    if (!vfsPermitted(sfsModeOf(parentInode), sfsUidOf(parentInode), sfsGidOf(parentInode),
                       currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
        return -1;
    if (dirFind(parentInode, leaf, strlen(leaf)) >= 0)
        return -1;

    int32 newInode = allocInodeNum();
    if (newInode < 0)
        return -1;

    SfsInode inode;
    memset(&inode, 0, sizeof(inode));
    inode.type = type;
    inode.linkCount = 1;
    inode.mtime = inode.ctime = (uint32)rtcNow();
    inode.mode = (type == sfsTypeDir) ? fsModeDefaultDir : fsModeDefaultFile;
    inode.uid = currentThreadUid();
    inode.gid = currentThreadGid();
    if (!writeInodeRaw(newInode, &inode)) {
        freeInodeNum(newInode);
        return -1;
    }

    if (!dirAdd(parentInode, leaf, strlen(leaf), newInode, type)) {
        freeInodeNum(newInode);
        return -1;
    }
    return newInode;
}

static bool readSymlinkTarget(int32 inodeNum, char* buf, uint64 cap)
{
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return false;
    if (inode.type != sfsTypeSymlink)
        return false;
    if (cap == 0 || (uint64)inode.size >= cap)
        return false;

    int64 got = sfsReadAt(inodeNum, 0, buf, inode.size);
    if (got != (int64)inode.size)
        return false;
    buf[inode.size] = 0;
    return true;
}

/* Walks path one component at a time from the root, exactly like the old
   flat sfsLookup used to, except a symlink component (other than possibly
   the last one, depending on followFinal) is resolved by re-running this
   same walk on its target and splicing the result back in as `cur` before
   continuing with whatever of the original path remains. depth counts hops
   already taken across that splicing, so a cycle (or merely a very long
   chain) fails cleanly with -1 instead of recursing without bound. */
static int32 lookupFrom(const char* path, bool followFinal, int depth)
{
    if (!isMounted)
        return -1;
    if (depth > sfsMaxSymlinkHops)
        return -1;

    int32 cur = sfsRootInode;
    const char* p = path ? path : "";
    while (*p == '/')
        p++;

    while (*p) {
        const char* start = p;
        while (*p && *p != '/')
            p++;
        uint64 len = (uint64)(p - start);
        if (len == 0 || len >= sfsNameMax)
            return -1;
        if (sfsTypeOf(cur) != sfsTypeDir)
            return -1;
        /* Real POSIX search permission on every directory ANCESTOR walked
           through, mirroring vfs.c's lookupNode's own equivalent check for
           tmpfs -- resolving a name inside `cur` needs the owner-or-other
           exec bit for the calling thread's uid, not just "cur is a
           directory". */
        if (!vfsPermitted(sfsModeOf(cur), sfsUidOf(cur), sfsGidOf(cur),
                           currentThreadUid(), currentThreadGid(), fsModeOwnerExec))
            return -1;
        int32 child = dirFind(cur, start, len);
        if (child < 0)
            return -1;

        while (*p == '/')
            p++;
        bool isLast = (*p == 0);

        if (sfsTypeOf(child) == sfsTypeSymlink && (!isLast || followFinal)) {
            char target[sfsSymlinkTargetMax + 1];
            if (!readSymlinkTarget(child, target, sizeof(target)))
                return -1;
            int32 resolved = lookupFrom(target, true, depth + 1);
            if (resolved < 0)
                return -1;
            child = resolved;
        }

        cur = child;
    }
    return cur;
}

/* --- public API --- */

bool sfsFormat(void)
{
    if (!ataPresent())
        return false;

    uint64 sectors = ataSectorCount();
    if (sectors <= sfsBaseLba + sfsReserveEnd + 64)
        return false;

    SfsSuperblock nsb;
    memset(&nsb, 0, sizeof(nsb));
    nsb.magic = sfsMagic;
    nsb.blockSize = sfsBlockSize;
    nsb.totalBlocks = (uint32)(sectors - sfsBaseLba - sfsReserveEnd);
    nsb.inodeCount = sfsMaxNodes;

    nsb.inodeBitmapStart = 1;
    nsb.inodeBitmapBlocks = (nsb.inodeCount + 8 * sfsBlockSize - 1) / (8 * sfsBlockSize);
    if (nsb.inodeBitmapBlocks == 0)
        nsb.inodeBitmapBlocks = 1;

    nsb.blockBitmapStart = nsb.inodeBitmapStart + nsb.inodeBitmapBlocks;
    nsb.blockBitmapBlocks = (nsb.totalBlocks + 8 * sfsBlockSize - 1) / (8 * sfsBlockSize);
    if (nsb.blockBitmapBlocks == 0)
        nsb.blockBitmapBlocks = 1;

    nsb.inodeTableStart = nsb.blockBitmapStart + nsb.blockBitmapBlocks;
    uint32 inodesPerBlock = sfsBlockSize / sizeof(SfsInode);
    nsb.inodeTableBlocks = (nsb.inodeCount + inodesPerBlock - 1) / inodesPerBlock;

    nsb.dataStart = nsb.inodeTableStart + nsb.inodeTableBlocks;
    nsb.rootInode = sfsRootInode;
    if (nsb.dataStart >= nsb.totalBlocks)
        return false;

    /* Committed to actually wiping from here on (every earlier return above
       is a rejected format that touched nothing) -- every inode number's
       old meaning is about to become garbage, so anything that cached one
       from before this point must be told apart from anything cached
       after. */
    formatGeneration++;

    if (inodeBitmap) {
        kfree(inodeBitmap);
        inodeBitmap = nullptr;
    }
    if (blockBitmap) {
        kfree(blockBitmap);
        blockBitmap = nullptr;
    }

    inodeBitmap = (uint8*)kmalloc((uint64)nsb.inodeBitmapBlocks * sfsBlockSize);
    blockBitmap = (uint8*)kmalloc((uint64)nsb.blockBitmapBlocks * sfsBlockSize);
    if (!inodeBitmap || !blockBitmap) {
        if (inodeBitmap) kfree(inodeBitmap);
        if (blockBitmap) kfree(blockBitmap);
        inodeBitmap = nullptr;
        blockBitmap = nullptr;
        return false;
    }
    memset(inodeBitmap, 0, (uint64)nsb.inodeBitmapBlocks * sfsBlockSize);
    memset(blockBitmap, 0, (uint64)nsb.blockBitmapBlocks * sfsBlockSize);

    sb = nsb;
    isMounted = true;

    uint8 zeroBlk[sfsBlockSize];
    memset(zeroBlk, 0, sizeof(zeroBlk));
    for (uint32 i = 0; i < sb.inodeTableBlocks; i++)
        blockWrite(sb.inodeTableStart + i, zeroBlk);

    bitSet(inodeBitmap, 0);
    bitSet(inodeBitmap, sfsRootInode);

    SfsInode rootInode;
    memset(&rootInode, 0, sizeof(rootInode));
    rootInode.type = sfsTypeDir;
    rootInode.linkCount = 1;
    rootInode.mtime = rootInode.ctime = (uint32)rtcNow();
    rootInode.mode = fsModeDefaultDir;
    rootInode.uid = currentThreadUid();
    rootInode.gid = currentThreadGid();
    writeInodeRaw(sfsRootInode, &rootInode);

    flushInodeBitmap();
    flushBlockBitmap();
    flushSuperblock();
    return true;
}

bool sfsMount(void)
{
    if (!ataPresent())
        return false;

    uint8 buf[sfsBlockSize];
    if (!ataReadSectors(sfsBaseLba, 1, buf))
        return false;

    SfsSuperblock onDisk;
    memcpy(&onDisk, buf, sizeof(onDisk));
    if (onDisk.magic != sfsMagic)
        return sfsFormat();

    sb = onDisk;

    if (inodeBitmap) {
        kfree(inodeBitmap);
        inodeBitmap = nullptr;
    }
    if (blockBitmap) {
        kfree(blockBitmap);
        blockBitmap = nullptr;
    }

    inodeBitmap = (uint8*)kmalloc((uint64)sb.inodeBitmapBlocks * sfsBlockSize);
    blockBitmap = (uint8*)kmalloc((uint64)sb.blockBitmapBlocks * sfsBlockSize);
    if (!inodeBitmap || !blockBitmap)
        return false;

    for (uint32 i = 0; i < sb.inodeBitmapBlocks; i++)
        if (!blockRead(sb.inodeBitmapStart + i, inodeBitmap + (uint64)i * sfsBlockSize))
            return false;
    for (uint32 i = 0; i < sb.blockBitmapBlocks; i++)
        if (!blockRead(sb.blockBitmapStart + i, blockBitmap + (uint64)i * sfsBlockSize))
            return false;

    isMounted = true;
    return true;
}

void sfsUnmount(void)
{
    if (!isMounted)
        return;
    flushInodeBitmap();
    flushBlockBitmap();
    flushSuperblock();
    if (inodeBitmap) {
        kfree(inodeBitmap);
        inodeBitmap = nullptr;
    }
    if (blockBitmap) {
        kfree(blockBitmap);
        blockBitmap = nullptr;
    }
    isMounted = false;
}

bool sfsMounted(void)
{
    return isMounted;
}

uint64 sfsCapacityBytes(void)
{
    if (!isMounted)
        return 0;
    return (uint64)(sb.totalBlocks - sb.dataStart) * sfsBlockSize;
}

uint64 sfsFreeDataBlocks(void)
{
    if (!isMounted)
        return 0;
    uint32 dataBlockCount = sb.totalBlocks - sb.dataStart;
    uint64 free = 0;
    for (uint32 i = 0; i < dataBlockCount; i++)
        if (!bitTest(blockBitmap, i))
            free++;
    return free;
}

int32 sfsLookup(const char* path)
{
    return lookupFrom(path, true, 0);
}

int32 sfsLookupNoFollow(const char* path)
{
    return lookupFrom(path, false, 0);
}

int32 sfsCreateFile(const char* path)
{
    return createNode(path, sfsTypeFile);
}

int32 sfsCreateDir(const char* path)
{
    return createNode(path, sfsTypeDir);
}

int32 sfsCreateFifo(const char* path)
{
    return createNode(path, sfsTypeFifo);
}

uint64 sfsGeneration(void)
{
    return formatGeneration;
}

int32 sfsCreateSymlink(const char* path, const char* target)
{
    if (!isMounted || !target)
        return -1;
    uint64 targetLen = strlen(target);
    if (targetLen == 0 || targetLen > sfsSymlinkTargetMax)
        return -1;

    int32 newInode = createNode(path, sfsTypeSymlink);
    if (newInode < 0)
        return -1;

    if (sfsWriteAt(newInode, 0, target, targetLen) != (int64)targetLen) {
        char parentBuf[256];
        char leaf[sfsNameMax];
        if (splitPath(path, parentBuf, sizeof(parentBuf), leaf, sizeof(leaf))) {
            int32 parentInode = sfsLookup(parentBuf);
            if (parentInode >= 0)
                dirRemove(parentInode, leaf, strlen(leaf));
        }
        freeInodeNum(newInode);
        return -1;
    }
    return newInode;
}

bool sfsReadlink(int32 inodeNum, char* buf, uint64 cap)
{
    if (!isMounted || inodeNum <= 0)
        return false;
    return readSymlinkTarget(inodeNum, buf, cap);
}

int32 sfsLink(const char* existingPath, const char* newPath)
{
    if (!isMounted)
        return -1;

    int32 target = sfsLookup(existingPath);
    if (target < 0)
        return -1;

    SfsInode inode;
    if (!readInode(target, &inode))
        return -1;
    if (inode.type == sfsTypeDir)
        return -1;

    char parentBuf[256];
    char leaf[sfsNameMax];
    if (!splitPath(newPath, parentBuf, sizeof(parentBuf), leaf, sizeof(leaf)))
        return -1;

    int32 parentInode = sfsLookup(parentBuf);
    if (parentInode < 0 || sfsTypeOf(parentInode) != sfsTypeDir)
        return -1;
    /* Same parent-directory write check as createNode/sfsRemove above --
       adding a new hardlink NAME needs write permission on the PARENT of
       the new name, not on the existing target being linked to. */
    if (!vfsPermitted(sfsModeOf(parentInode), sfsUidOf(parentInode), sfsGidOf(parentInode),
                       currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
        return -1;
    if (dirFind(parentInode, leaf, strlen(leaf)) >= 0)
        return -1;

    inode.linkCount++;
    inode.ctime = (uint32)rtcNow();
    if (!writeInodeRaw(target, &inode))
        return -1;

    if (!dirAdd(parentInode, leaf, strlen(leaf), target, inode.type)) {
        inode.linkCount--;
        writeInodeRaw(target, &inode);
        return -1;
    }
    return target;
}

uint32 sfsLinkCountOf(int32 inodeNum)
{
    if (!isMounted || inodeNum <= 0)
        return 0;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return 0;
    return inode.linkCount;
}

bool sfsRemove(const char* path)
{
    if (!isMounted)
        return false;

    char parentBuf[256];
    char leaf[sfsNameMax];
    if (!splitPath(path, parentBuf, sizeof(parentBuf), leaf, sizeof(leaf)))
        return false;

    int32 parentInode = sfsLookup(parentBuf);
    if (parentInode < 0)
        return false;
    /* Same parent-directory write check as createNode above -- removing an
       entry needs write permission on the PARENT, not the entry itself,
       mirroring vfs.c's vfsUnlink/vfsRmdir's own equivalent tmpfs check. */
    if (!vfsPermitted(sfsModeOf(parentInode), sfsUidOf(parentInode), sfsGidOf(parentInode),
                       currentThreadUid(), currentThreadGid(), fsModeOwnerWrite))
        return false;
    int32 target = dirFind(parentInode, leaf, strlen(leaf));
    if (target < 0)
        return false;

    SfsInode inode;
    if (!readInode(target, &inode))
        return false;
    if (inode.type == sfsTypeDir && inode.size > 0)
        return false;

    if (!dirRemove(parentInode, leaf, strlen(leaf)))
        return false;

    if (inode.linkCount > 1) {
        inode.linkCount--;
        inode.ctime = (uint32)rtcNow();
        return writeInodeRaw(target, &inode);
    }

    if (!sfsTruncateInode(&inode, target, 0))
        return false;
    freeInodeNum(target);
    return true;
}

uint64 sfsSizeOf(int32 inodeNum)
{
    if (!isMounted || inodeNum <= 0)
        return 0;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return 0;
    return inode.size;
}

uint32 sfsTypeOf(int32 inodeNum)
{
    if (!isMounted || inodeNum <= 0)
        return sfsTypeFree;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return sfsTypeFree;
    return inode.type;
}

uint64 sfsMtimeOf(int32 inodeNum)
{
    if (!isMounted || inodeNum <= 0)
        return 0;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return 0;
    return inode.mtime;
}

uint64 sfsCtimeOf(int32 inodeNum)
{
    if (!isMounted || inodeNum <= 0)
        return 0;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return 0;
    return inode.ctime;
}

uint32 sfsModeOf(int32 inodeNum)
{
    if (!isMounted || inodeNum <= 0)
        return 0;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return 0;
    return inode.mode;
}

bool sfsChmod(int32 inodeNum, uint32 mode)
{
    if (!isMounted || inodeNum <= 0)
        return false;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return false;
    inode.mode = mode;
    inode.ctime = (uint32)rtcNow();
    return writeInodeRaw(inodeNum, &inode);
}

uint32 sfsUidOf(int32 inodeNum)
{
    if (!isMounted || inodeNum <= 0)
        return 0;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return 0;
    return inode.uid;
}

bool sfsChown(int32 inodeNum, uint32 newUid)
{
    if (!isMounted || inodeNum <= 0)
        return false;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return false;
    inode.uid = newUid;
    inode.ctime = (uint32)rtcNow();
    return writeInodeRaw(inodeNum, &inode);
}

uint32 sfsGidOf(int32 inodeNum)
{
    if (!isMounted || inodeNum <= 0)
        return 0;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return 0;
    return inode.gid;
}

bool sfsChgrp(int32 inodeNum, uint32 newGid)
{
    if (!isMounted || inodeNum <= 0)
        return false;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return false;
    inode.gid = newGid;
    inode.ctime = (uint32)rtcNow();
    return writeInodeRaw(inodeNum, &inode);
}

bool sfsTruncate(int32 inodeNum, uint64 newSize)
{
    if (!isMounted || inodeNum <= 0 || newSize > 0xFFFFFFFFu)
        return false;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return false;
    if (inode.type != sfsTypeFile)
        return false;
    return sfsTruncateInode(&inode, inodeNum, (uint32)newSize);
}

int64 sfsReadAt(int32 inodeNum, uint64 offset, void* buffer, uint64 length)
{
    if (!isMounted || inodeNum <= 0)
        return -1;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return -1;
    if (inode.type != sfsTypeFile && inode.type != sfsTypeDir && inode.type != sfsTypeSymlink)
        return -1;
    if (offset >= inode.size)
        return 0;

    uint64 want = length;
    if (want > (uint64)inode.size - offset)
        want = (uint64)inode.size - offset;

    uint8* dst = (uint8*)buffer;
    uint64 done = 0;
    while (done < want) {
        uint64 pos = offset + done;
        uint32 logicalBlock = (uint32)(pos / sfsBlockSize);
        uint32 within = (uint32)(pos % sfsBlockSize);
        uint32 chunk = sfsBlockSize - within;
        if ((uint64)chunk > want - done)
            chunk = (uint32)(want - done);

        uint32 blockAbs;
        if (getBlockForRead(&inode, logicalBlock, &blockAbs)) {
            uint8 blockBuf[sfsBlockSize];
            if (!blockRead(blockAbs, blockBuf))
                break;
            memcpy(dst + done, blockBuf + within, chunk);
        } else {
            memset(dst + done, 0, chunk);
        }
        done += chunk;
    }
    return (int64)done;
}

int64 sfsWriteAt(int32 inodeNum, uint64 offset, const void* buffer, uint64 length)
{
    if (!isMounted || inodeNum <= 0)
        return -1;
    SfsInode inode;
    if (!readInode(inodeNum, &inode))
        return -1;
    if (inode.type != sfsTypeFile && inode.type != sfsTypeDir && inode.type != sfsTypeSymlink)
        return -1;
    if (length == 0)
        return 0;

    if (offset > inode.size) {
        if (!sfsWriteRaw(&inode, inodeNum, inode.size, nullptr, offset - inode.size))
            return -1;
    }
    if (!sfsWriteRaw(&inode, inodeNum, offset, buffer, length))
        return -1;
    return (int64)length;
}

bool sfsReaddirAt(int32 dirInode, uint32 index, char* nameOut, uint32 nameCap, uint32* typeOut)
{
    if (!isMounted || sfsTypeOf(dirInode) != sfsTypeDir)
        return false;

    SfsDirEntry entry;
    uint64 pos = (uint64)index * sizeof(SfsDirEntry);
    if (sfsReadAt(dirInode, pos, &entry, sizeof(entry)) != (int64)sizeof(entry))
        return false;

    uint64 nlen = strlen(entry.name);
    if (nlen >= nameCap)
        nlen = nameCap - 1;
    memcpy(nameOut, entry.name, nlen);
    nameOut[nlen] = 0;
    if (typeOut)
        *typeOut = entry.type;
    return true;
}
