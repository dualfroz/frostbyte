#include "isr.h"
#include "console.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "sched.h"
#include "ports.h"
#include "signal.h"
#include "mmap.h"

static const char* exceptionNames[32] = {
    "divide by zero",
    "debug",
    "non maskable interrupt",
    "breakpoint",
    "overflow",
    "bound range exceeded",
    "invalid opcode",
    "device not available",
    "double fault",
    "coprocessor segment overrun",
    "invalid tss",
    "segment not present",
    "stack segment fault",
    "general protection fault",
    "page fault",
    "reserved",
    "x87 floating point",
    "alignment check",
    "machine check",
    "simd floating point",
    "virtualization",
    "control protection",
    "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved",
    "hypervisor injection", "vmm communication"
};

static void panicOnException(Registers* regs)
{
    uint64 faultAddress = 0;
    if (regs->intNo == 14)
        __asm__ volatile("mov %%cr2, %0" : "=r"(faultAddress));

    kprintf("\n[PANIC] exception %lu (%s)\n", regs->intNo, exceptionNames[regs->intNo]);
    kprintf("  errCode=%lx rip=%lx cs=%lx rflags=%lx\n",
            regs->errCode, regs->rip, regs->cs, regs->rflags);
    if (regs->intNo == 14)
        kprintf("  cr2=%lx (faulting address)\n", faultAddress);
    kprintf("  rax=%lx rbx=%lx rcx=%lx rdx=%lx\n", regs->rax, regs->rbx, regs->rcx, regs->rdx);
    kprintf("  system halted\n");

    for (;;)
        __asm__ volatile("cli; hlt");
}

/* Runs at every single return-to-ring3 point this kernel has (a completed
   syscall, a timer reschedule, any other IRQ resuming whoever it
   interrupted) -- the one choke point isrDispatch funnels its result
   through below, regardless of which of those three paths produced it.
   `rsp` is whatever Registers* the caller is about to resume; if that
   context is not ring 3 (a kernel thread, e.g. workerA/shell -- see
   sched.h's userCodeSelector|3 vs kernelCodeSelector), signals are not a
   concept that applies to it at all and this is a no-op. Otherwise
   currentThread() is guaranteed to be exactly the owner of `rsp` (schedule()
   already updated currentIndex before returning it; the syscall/other-IRQ
   paths never changed currentIndex away from whoever trapped in), so
   sigConsumePending always inspects the right thread's pending/blocked
   state. A default-disposition Terminate (or job-control Stop, sigTstp's
   default -- see signal.h) signal cannot be handled here directly -- it
   needs a NEW thread's rsp, which only schedule() can produce -- so the
   loop below reuses the exact same "flip state, then reschedule" pattern
   sysExit/sysSleep already use (threadExit for Terminate, threadStop for
   Stop), and loops again in case the freshly chosen thread also has a
   signal of its own waiting (e.g. two threads both made Runnable by the
   same timer tick and both signaled). */
static uint64 deliverPendingSignals(uint64 rsp)
{
    for (;;) {
        Registers* regs = (Registers*)rsp;
        if ((regs->cs & 3) != 3)
            return rsp;

        Thread* thread = currentThread();
        int signum = 0;
        SigOutcome outcome = sigConsumePending(thread, regs, &signum);

        if (outcome == sigOutcomeNone)
            return rsp;

        if (outcome == sigOutcomeHandler)
            return (uint64)regs;

        if (outcome == sigOutcomeStop) {
            kprintf("[sig]  pid=%u '%s' stopped by signal %d\n",
                    thread->id, thread->name, signum);
            threadStop(thread->id);
            rsp = schedule(rsp);
            continue;
        }

        kprintf("[sig]  pid=%u '%s' terminated by uncaught signal %d\n",
                thread->id, thread->name, signum);
        threadExit(128u + (uint64)signum);
        rsp = schedule(rsp);
    }
}

/* Attempts to resolve a ring-3 #PF through mmap.c's own demand-paging
   machinery: cr2 is the real faulting address (same register the panic
   path above already reads), errCode bit 1 (0x2) is the CPU's own
   "this was a write access" flag -- the exact distinction
   mmapFaultHandler needs to refuse a write against a read-only mapping
   instead of silently allowing it. Returns whatever mmapFaultHandler
   itself returns: true means a real frame is now mapped and the faulting
   instruction can simply be retried (the CPU never advanced rip past a
   #PF to begin with); false means this address is not a mapping this
   kernel's mmap.c even knows about, or the access violates its own prot
   bits -- the caller (isrDispatch below) turns that into a real SIGSEGV
   rather than the kernel-wide panic every OTHER exception still gets. */
static bool tryResolveUserPageFault(Registers* regs)
{
    /* errCode bit 4 (I/D) is the CPU's own "this fault was an instruction
       fetch" flag -- only ever set once paging.h's pageNoExecute is real
       hardware enforcement (IA32_EFER.NXE, see paging.c's
       pagingNxSupported/boot.S), meaning a PRESENT page's own NX bit
       refused execution. This can never be something mmapFaultHandler's
       demand-paging logic fixes: there is no lazy version of "make this
       page executable" the way there is for a not-yet-faulted-in page, and
       mmapFaultHandler's own "pagingResolve != 0 -> already resolved, just
       retry" shortcut (mmap.c) would otherwise see the very same present
       page, report success, and send the CPU straight back to retry the
       identical faulting instruction fetch -- an infinite fault loop that
       hangs the whole kernel rather than the one offending thread. Refuse
       outright instead; isrDispatch below turns this into a real SIGSEGV
       exactly like any other refusal, whether the address belongs to an
       mmap Vma, or (mmapFind would never even find it) a stack/brk page
       mapped NX by kmain.c/syscall.c/jobctl.c/sched.c. */
    if (regs->errCode & 0x10)
        return false;

    uint64 faultAddress;
    __asm__ volatile("mov %%cr2, %0" : "=r"(faultAddress));
    bool forWrite = (regs->errCode & 0x2) != 0;
    return mmapFaultHandler(currentThread(), faultAddress, forWrite);
}

uint64 isrDispatch(Registers* regs)
{
    /* A ring-3 (user-mode) page fault gets one real chance at recovery
       before falling through to the unconditional panic below: an
       instruction inside SOME OTHER thread's buggy user program should
       never be able to take the whole machine down with it, exactly like
       a real kernel's own per-process fault isolation -- this kernel's
       flat, single-shared-address-space design (see paging.c's own file
       comments) cannot isolate a bad ACCESS the way separate page tables
       would, but it can still isolate the CONSEQUENCE, by killing only the
       one thread that caused it. A kernel-mode fault (ring 0, cs&3==0 --
       this kernel's own bug, not a user program's) still panics
       unconditionally below, exactly as before this existed. */
    if (regs->intNo == 14 && (regs->cs & 3) == 3) {
        if (!tryResolveUserPageFault(regs))
            sigRaiseInfo(currentThreadId(), sigSegv, siCodeKernel, 0, 0);
        return deliverPendingSignals((uint64)regs);
    }

    if (regs->intNo < 32)
        panicOnException(regs);

    uint64 nextRsp;

    if (regs->intNo == 128) {
        nextRsp = syscallDispatch(regs);
    } else {
        uint8 irq = (uint8)(regs->intNo - 32);
        nextRsp = (uint64)regs;

        if (irq == 0) {
            pitTick();
            nextRsp = schedule((uint64)regs);
        } else if (irq == 1) {
            keyboardHandler();
        }

        picSendEoi(irq);
    }

    return deliverPendingSignals(nextRsp);
}
