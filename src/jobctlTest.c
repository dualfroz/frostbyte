#include "ktest.h"
#include "jobctl.h"
#include "sched.h"
#include "vfs.h"
#include "elf.h"

static uint64 binaryEntry(const char* path)
{
    VfsNode* node = vfsLookup(path);
    return node ? elfImageEntry(vfsFileData(node), vfsSize(node)) : 0;
}

static int slotHolders(uint64 slotBase)
{
    int holders = 0;
    for (uint32 i = 0; i < threadCount(); i++) {
        Thread* thread = threadAt(i);
        if (thread->state != threadUnused && thread->userSlotBase == slotBase)
            holders++;
    }
    return holders;
}

KTEST(threadUserSlotHeldByOtherIgnoresTheGivenOwner)
{
    uint64 slotBase = 0x710000000ull;
    uint64 entry = slotBase + 0x2000;
    uint32 id = threadCreateUser("slotOwner", entry, elfSlotStackTop(entry));
    KT_NE(t, id, threadInvalidId);

    KT_TRUE(t, threadUserSlotHeldByOther(slotBase, threadInvalidId));
    KT_TRUE(t, threadUserSlotHeldByOther(slotBase, currentThreadId()));
    KT_FALSE(t, threadUserSlotHeldByOther(slotBase, id));
    KT_FALSE(t, threadUserSlotHeldByOther(slotBase + elfSlotSize, threadInvalidId));

    KT_TRUE(t, threadDiscard(id));
    KT_FALSE(t, threadUserSlotHeldByOther(slotBase, threadInvalidId));
}

KTEST(threadImageSlotBusyFollowsTheImageEntrySlot)
{
    VfsNode* node = vfsLookup("/bin/hello");
    KT_NOTNULL(t, node);
    if (!node)
        return;
    const void* image = vfsFileData(node);
    uint64 size = vfsSize(node);
    uint64 entry = elfImageEntry(image, size);
    KT_NE(t, entry, 0);
    KT_FALSE(t, threadImageSlotBusy(image, size, threadInvalidId));

    uint32 holder = threadCreateUser("helloHolder", entry, elfSlotStackTop(entry));
    KT_NE(t, holder, threadInvalidId);
    KT_TRUE(t, threadImageSlotBusy(image, size, threadInvalidId));
    KT_FALSE(t, threadImageSlotBusy(image, size, holder));
    KT_FALSE(t, threadImageSlotBusy(image, 8, threadInvalidId));

    KT_TRUE(t, threadDiscard(holder));
    KT_FALSE(t, threadImageSlotBusy(image, size, threadInvalidId));
}

KTEST(jobctlRefusesABinaryWhoseSlotIsHeld)
{
    uint64 entry = binaryEntry("/bin/hello");
    KT_NE(t, entry, 0);
    KT_FALSE(t, jobctlBinaryBusy("/bin/hello"));
    KT_FALSE(t, jobctlBinaryBusy("/bin/does-not-exist"));

    uint32 holder = threadCreateUser("helloHolder", entry, elfSlotStackTop(entry));
    KT_NE(t, holder, threadInvalidId);
    KT_TRUE(t, jobctlBinaryBusy("/bin/hello"));

    const char* argv[1] = { "/bin/hello" };
    KT_EQ(t, jobctlSpawnPiped("/bin/hello", argv, 1, -1, -1, threadInvalidId), threadInvalidId);
    uint32 background = threadInvalidId;
    KT_FALSE(t, jobctlRunBackground("/bin/hello", argv, 1, &background));
    KT_EQ(t, background, threadInvalidId);
    KT_EQ(t, slotHolders(elfSlotBase(entry)), 1);

    KT_TRUE(t, threadDiscard(holder));
    KT_FALSE(t, jobctlBinaryBusy("/bin/hello"));
    KT_EQ(t, slotHolders(elfSlotBase(entry)), 0);
}
