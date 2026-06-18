#include "types.h"
#include "console.h"
#include "gdt.h"
#include "idt.h"
#include "tss.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "pmm.h"
#include "paging.h"
#include "heap.h"
#include "sched.h"
#include "shell.h"
#include "multiboot2.h"
#include "initrd.h"
#include "vfs.h"
#include "elf.h"

#define multibootBootMagic 0x36D76289
#define heapSize (8 * 1024 * 1024)
#define timerFrequency 100
#define userStackSize 0x4000

static uint8 bootstrapKernelStack[16384] __attribute__((aligned(16)));

static void banner(void)
{
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  frostbyte kernel  x86_64  long mode\n");
    kprintf("  wave 2: userspace, ring 3, syscalls\n");
    kprintf("========================================\n\n");
}

static void shellThread(void)
{
    shellRun();
}

static void workerA(void)
{
    uint64 iterations = 0;
    uint64 nextLog = 50;
    for (;;) {
        iterations++;
        if (pitTicks() >= nextLog) {
            kprintf("[workerA] tick=%lu iterations=%lu\n", pitTicks(), iterations);
            nextLog += 100;
        }
    }
}

static void workerB(void)
{
    uint64 iterations = 0;
    uint64 nextLog = 90;
    for (;;) {
        iterations++;
        if (pitTicks() >= nextLog) {
            kprintf("[workerB] tick=%lu iterations=%lu\n", pitTicks(), iterations);
            nextLog += 100;
        }
    }
}

static void runHeapTest(void)
{
    kprintf("[heap] free-list allocator self-test:\n");

    void* a = kmalloc(100);
    void* b = kmalloc(200);
    void* c = kmalloc(100);
    kprintf("[heap]   alloc a=%p b=%p c=%p, live blocks=%lu\n",
            a, b, c, heapBlockCount());

    kfree(b);
    void* d = kmalloc(150);
    kprintf("[heap]   free(b) then malloc(150) -> d=%p, reused b: %s\n",
            d, d == b ? "yes" : "no");

    uint64 beforeCoalesce = heapLargestFree();
    kfree(a);
    kfree(c);
    kfree(d);
    uint64 afterCoalesce = heapLargestFree();

    kprintf("[heap]   largest free %lu KiB -> %lu KiB after freeing a,c,d\n",
            beforeCoalesce / 1024, afterCoalesce / 1024);
    kprintf("[heap]   coalesce %s, %lu KiB free of %lu KiB, blocks=%lu\n",
            afterCoalesce > beforeCoalesce ? "PASS" : "FAIL",
            heapFreeBytes() / 1024, heapTotal() / 1024, heapBlockCount());
}

static void runPagingTest(void)
{
    uint64 testVirtual = 0xC0000000;
    uint64 frame = pmmAlloc();

    if (frame == 0) {
        kprintf("[paging] no free frame for test\n");
        return;
    }

    if (!pagingMap(testVirtual, frame, pagePresent | pageWrite)) {
        kprintf("[paging] mapping failed\n");
        return;
    }

    volatile uint64* probe = (volatile uint64*)testVirtual;
    *probe = 0xCAFEBABEDEADBEEFull;

    uint64 resolved = pagingResolve(testVirtual);
    if (*probe == 0xCAFEBABEDEADBEEFull && resolved == frame)
        kprintf("[paging] mapped %p -> phys %lx, readback OK\n", (void*)testVirtual, frame);
    else
        kprintf("[paging] readback FAILED (value=%lx resolved=%lx)\n", *probe, resolved);
}

static void spawnUser(const char* path, const char* name, uint64 stackTop, uint64 brkStart)
{
    VfsNode* node = vfsLookup(path);
    if (!node) {
        kprintf("[user] '%s' not found in vfs\n", path);
        return;
    }

    const void* image = vfsFileData(node);
    uint64 size = vfsSize(node);
    uint64 entry = elfLoad(image, size);
    if (entry == 0) {
        kprintf("[user] '%s' failed to load\n", path);
        return;
    }

    pagingMapUserRange(stackTop - userStackSize, userStackSize);
    uint32 pid = threadCreateUser(name, entry, stackTop);
    if (brkStart)
        threadSetBrk(pid, brkStart);

    kprintf("[user] loaded '%s' pid=%u entry=%p stackTop=%p (%lu bytes)\n",
            name, pid, (void*)entry, (void*)stackTop, size);
}

void kmain(uint32 magic, uint64 multibootInfo)
{
    consoleInit();
    banner();

    if (magic == multibootBootMagic)
        kprintf("[boot] multiboot2 magic verified (%x)\n", magic);
    else
        kprintf("[boot] unexpected boot magic %x\n", magic);

    gdtInit();
    kprintf("[gdt]  kernel + user segments, tss slot reserved\n");

    idtInit();
    kprintf("[idt]  256 vectors (32 exceptions, 16 irqs, int 0x80 syscall)\n");

    picRemap();
    for (uint8 irq = 0; irq < 16; irq++)
        picSetMask(irq);
    picClearMask(0);
    picClearMask(1);
    kprintf("[pic]  remapped to 0x20-0x2f, irq0 and irq1 unmasked\n");

    uint64 moduleStart = 0;
    uint64 moduleEnd = 0;
    bool haveModule = multibootFindModule(multibootInfo, &moduleStart, &moduleEnd);

    pmmInit(multibootInfo, moduleEnd);
    kprintf("[pmm]  %lu KiB usable, %lu frames, %lu free after kernel\n",
            pmmTotalBytes() / 1024, pmmTotalFrames(), pmmFreeFrames());

    uint64 heapStart = pmmReservedEnd();
    pmmReserve(heapStart, heapSize);
    heapInit(heapStart, heapSize);
    kprintf("[heap] free-list allocator at %p, %u KiB\n", (void*)heapStart, heapSize / 1024);

    runHeapTest();
    runPagingTest();

    pitInit(timerFrequency);
    kprintf("[pit]  timer at %u Hz\n", timerFrequency);

    keyboardInit();
    kprintf("[kbd]  ps/2 keyboard driver ready\n");

    schedInit();
    tssInit((uint64)(bootstrapKernelStack + sizeof(bootstrapKernelStack)));
    kprintf("[tss]  loaded, rsp0 bootstrap stack installed\n");

    vfsInit();
    if (haveModule)
        vfsMountUstar(moduleStart, moduleEnd);
    else
        kprintf("[vfs]  no multiboot2 module present\n");
    vfsMountTmpfs("/tmp");

    threadCreate("shell", shellThread);
    threadCreate("workerA", workerA);
    threadCreate("workerB", workerB);

    kprintf("\n[boot] launching userland programs from the ramdisk into ring 3\n");
    spawnUser("/bin/hello", "hello", 0x48000000, 0);
    spawnUser("/bin/counter", "counter", 0x58000000, 0);
    spawnUser("/bin/sbrktest", "sbrktest", 0x68000000, 0x64000000);
    spawnUser("/bin/fileread", "fileread", 0x78000000, 0);

    kprintf("[sched] %u threads total (idle, shell, 2 workers, user programs)\n",
            threadCount());
    kprintf("\n[boot] enabling interrupts, starting preemptive scheduler\n\n");

    schedStart();
}
