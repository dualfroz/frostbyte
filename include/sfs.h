#ifndef FROSTBYTE_SFS_H
#define FROSTBYTE_SFS_H

#include "types.h"

/*
 * SFS ("simple filesystem"): a real, persistent, inode-based filesystem
 * living on the ATA block device (ata.h), starting at LBA sfsBaseLba. Unlike
 * the VFS tmpfs tree (vfs.c), which is pure kmalloc state that evaporates on
 * reboot, SFS state (superblock, bitmaps, inode table, file data) is fully
 * on disk: sfsUnmount()+sfsMount() drops every in-memory structure and
 * reconstructs it by reading the disk, the same way a real filesystem driver
 * would after a remount.
 *
 * Block size equals the sector size (512 bytes), so every filesystem block
 * is exactly one ATA sector transfer. sfsBaseLba (100) and the reserved
 * tail (16 sectors) keep this filesystem's LBA range clear of the raw
 * sector ranges ataTest.c/disktest.c already poke directly, so the two
 * layers never fight over the same bytes even though they share one disk.
 */

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

/* Formats the SFS region from scratch: fresh superblock, zeroed bitmaps and
   inode table, empty root directory. Destroys any existing SFS content in
   that LBA range. Returns false if no drive is present or the drive is too
   small to hold the fixed-size metadata. */
bool sfsFormat(void);

/* Mounts the existing on-disk filesystem, auto-formatting on first use (bad
   magic). Loads the inode/block bitmaps into memory; everything else (inode
   table, file data, directory entries) is read from disk on demand. */
bool sfsMount(void);

/* Flushes bitmaps/superblock to disk and frees in-memory state. A later
   sfsMount() reconstructs everything purely from what is on disk. */
void sfsUnmount(void);

bool sfsMounted(void);

/* Total bytes usable for file data (excludes reserved metadata blocks). */
uint64 sfsCapacityBytes(void);

/* Data blocks not currently allocated to any inode (bitmap free count).
   Mainly useful for tests to prove truncate/remove actually gives blocks
   back rather than merely detaching them from the inode. */
uint64 sfsFreeDataBlocks(void);

/* path is slash-separated and relative to the SFS root ("" or "/" is the
   root itself). Returns the inode number (>=1) or -1 if not found. Note:
   "." and ".." are not special-cased (deliberate scope limit: paths are
   always resolved from the root down, no relative-to-self hops).

   A symlink encountered anywhere along the path - including the final
   component - is transparently followed to its target and re-resolved from
   the root (real open()/stat() semantics), up to sfsMaxSymlinkHops hops;
   beyond that (a cycle, or a chain that is simply too long) resolution fails
   with -1 instead of recursing forever. Use sfsLookupNoFollow to inspect a
   symlink itself rather than what it points at. */
int32 sfsLookup(const char* path);

/* Same resolution as sfsLookup, except a symlink at the FINAL path component
   is returned as itself instead of being followed (the lstat()/readlink()
   convention). Symlinks at any earlier component are still followed, since
   there is no other way to keep descending through them. */
int32 sfsLookupNoFollow(const char* path);

int32 sfsCreateFile(const char* path);
int32 sfsCreateDir(const char* path);

/* Creates a persistent named-pipe entry at `path`: a real, on-disk directory
   entry of type sfsTypeFifo that survives sfsUnmount()+sfsMount() (and a
   real reboot, unlike the VFS tmpfs-tree fifos vfsMkfifo already supports
   under e.g. "/tmp") exactly the way a real filesystem's mkfifo(2) special
   file does. The inode carries no data blocks -- sfsReadAt/sfsWriteAt
   already refuse anything that is not sfsTypeFile/Dir/Symlink, so a fifo
   inode simply has size 0 forever, matching real Unix (mkfifo persists the
   NAME and TYPE of the rendezvous point, never any buffered bytes: those
   only ever exist in memory while some process holds the pipe open, and
   are gone the moment every process does, reboot or not). vfs.c is
   responsible for the actually-live half of a fifo (attaching real
   pipe.h byte-queue handles to opens of this path, see vfsOpen) --
   sfs.c itself stays entirely ignorant of pipe.h, exactly as it already is
   of every other higher-layer concern. Returns the new inode number, or -1
   on the same failures as sfsCreateFile/sfsCreateDir (parent missing/not a
   writable dir, name already taken, tables full). */
int32 sfsCreateFifo(const char* path);

/* Monotonically increasing counter, bumped exactly once every time
   sfsFormat() actually wipes the on-disk filesystem (whether called
   directly, e.g. every sfsTest.c test's own clean-slate setup, or
   implicitly by sfsMount() the first time it sees a bad magic) -- never
   bumped by a plain sfsMount()/sfsUnmount() cycle that finds a valid,
   unchanged filesystem, since inode numbers keep meaning the same thing
   across that kind of remount. Exists so a higher layer that caches
   something keyed by inode number across calls (vfs.c's SFS fifo binding
   table, see vfsOpen/vfsMkfifo/vfsUnlink in vfs.c) can tell "this cached
   entry still refers to the same on-disk fifo I bound it to" apart from
   "the whole filesystem underneath it was wiped and reformatted since,
   inode 7 might be a completely different file/dir/fifo/nothing now" --
   without sfs.c ever having to know that cache exists. */
uint64 sfsGeneration(void);

/* Creates a symbolic link at `path` whose target is the literal string
   `target` - never validated against anything, so a symlink may point at a
   path that does not exist yet (or ever), exactly like a real filesystem.
   Returns the new inode number, or -1 on failure (parent missing, name
   already taken, target too long, or the inode/block tables are full). */
int32 sfsCreateSymlink(const char* path, const char* target);

/* Reads the target string of a symlink inode into buf, null-terminated.
   Fails (returns false, buf left untouched) if inodeNum is not a symlink or
   if the stored target does not fit in cap bytes including the terminator -
   never silently truncates. */
bool sfsReadlink(int32 inodeNum, char* buf, uint64 cap);

/* Creates a second directory entry (newPath) for the same inode an existing
   file or symlink resolves to (existingPath is followed through symlinks,
   same as open()/stat() - the new name refers to whatever it ultimately
   points at, not the symlink itself). Directories cannot be hardlinked
   (matches POSIX link(2): would make the directory tree no longer a tree).
   Returns the shared inode number, or -1 on failure (existingPath missing,
   a directory, newPath's parent missing, or newPath already taken). */
int32 sfsLink(const char* existingPath, const char* newPath);

/* Number of directory entries currently pointing at this inode. 0 for an
   invalid/unmounted lookup, otherwise always >=1 for anything sfsLookup can
   still reach. */
uint32 sfsLinkCountOf(int32 inodeNum);

/* Removes a file, or a directory only if it is empty. A hardlinked file's
   data and inode survive until its link count reaches zero -- only the
   named directory entry is guaranteed to disappear immediately. */
bool sfsRemove(const char* path);

uint64 sfsSizeOf(int32 inodeNum);
uint32 sfsTypeOf(int32 inodeNum);

/* Real wall-clock timestamps (rtc.h's rtcNow(), seconds since the Unix
   epoch), stored on disk in the inode itself so they survive
   sfsUnmount()+sfsMount() like everything else about the inode. mtime is
   last content modification (sfsWriteAt, sfsTruncate); ctime is last inode
   metadata change (mtime changes are also metadata changes, so every mtime
   update bumps ctime too, plus link-count changes from sfsLink/sfsRemove
   bump ctime alone, matching real inode semantics). Both are stamped at
   creation, so a freshly created node has mtime == ctime. Return 0 for an
   invalid/unmounted lookup, same convention as sfsSizeOf/sfsTypeOf. */
uint64 sfsMtimeOf(int32 inodeNum);
uint64 sfsCtimeOf(int32 inodeNum);

/* Real owner rwx permission bits (fs.h's fsModeOwner*), stored on disk in
   the inode like every other field above -- survives sfsUnmount()+
   sfsMount(). Stamped at creation with a type-based default (createNode:
   fsModeDefaultDir for a directory, fsModeDefaultFile for anything else)
   and changed only by sfsChmod. sfsModeOf returns 0 for an invalid/
   unmounted lookup, same convention as sfsSizeOf/sfsTypeOf. sfsChmod
   overwrites the whole value (no symbolic +x-style merge, matching real
   chmod(2)'s own numeric-mode form) and bumps ctime like any other
   metadata-only mutation (sfsLink's linkCount bump does the same) --
   returns false for an invalid/unmounted inodeNum, true otherwise. */
uint32 sfsModeOf(int32 inodeNum);
bool sfsChmod(int32 inodeNum, uint32 mode);

/* Real owner identity (fs.h's FbStat.uid), stored on disk like every other
   inode field above -- survives sfsUnmount()+sfsMount(). Stamped once, at
   creation (createNode: sched.h's currentThreadUid(), the CALLER's real
   identity at the moment of the create/mkdir/symlink/mkfifo call), and
   afterward only ever changed by sfsChown (below). sfsUidOf returns 0 for
   an invalid/unmounted lookup, same convention as sfsModeOf. */
uint32 sfsUidOf(int32 inodeNum);

/* Real chown(2)'s on-disk half: overwrites the inode's owner uid outright
   and bumps ctime like sfsChmod's own metadata-only mutation does.
   Deliberately has NO privilege check of its own -- vfsChown (vfs.c) is the
   one real choke point that enforces the actual chown(2) privilege rule
   (root-only, real Unix's _POSIX_CHOWN_RESTRICTED default: not even a
   file's own owner may give it away), exactly like sfsChmod leaves
   ownership-vs-caller policy to vfsChmod. Returns false for an invalid/
   unmounted inodeNum, same convention as sfsChmod. */
bool sfsChown(int32 inodeNum, uint32 newUid);

/* Real group identity (fs.h's FbStat.gid), stored on disk like uid above --
   survives sfsUnmount()+sfsMount(). Stamped once, at creation (createNode:
   sched.h's currentThreadGid()), and afterward only ever changed by
   sfsChgrp (below). sfsGidOf returns 0 for an invalid/unmounted lookup,
   same convention as sfsUidOf. */
uint32 sfsGidOf(int32 inodeNum);

/* Real chgrp(2)'s on-disk half: overwrites the inode's group gid outright
   and bumps ctime, mirroring sfsChown's own shape exactly. Deliberately has
   NO privilege check of its own -- vfsChgrp (vfs.c) is the real choke point
   (owner may set only their own current gid, root may set any gid). Returns
   false for an invalid/unmounted inodeNum, same convention as sfsChown. */
bool sfsChgrp(int32 inodeNum, uint32 newGid);

/* Grows or shrinks a file in place, freeing/allocating data blocks as
   needed. */
bool sfsTruncate(int32 inodeNum, uint64 newSize);

/* Reads/writes inode-relative byte ranges. Writing past the current end of
   file zero-fills the gap first (no sparse holes), then writes the payload,
   growing the file and allocating blocks (direct, then single indirect,
   then double indirect, then triple indirect) as needed. Returns bytes
   transferred, or -1 on error (inode not a file, or the file has hit the
   direct+indirect+double+triple addressing limit, roughly 1 GiB). */
int64 sfsReadAt(int32 inodeNum, uint64 offset, void* buffer, uint64 length);
int64 sfsWriteAt(int32 inodeNum, uint64 offset, const void* buffer, uint64 length);

/* Lists directory entries by index (0, 1, 2, ...). Returns false past the
   last entry. */
bool sfsReaddirAt(int32 dirInode, uint32 index, char* nameOut, uint32 nameCap, uint32* typeOut);

#endif
