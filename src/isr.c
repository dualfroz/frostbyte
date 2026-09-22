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

static bool tryResolveUserPageFault(Registers* regs)
{
    if (regs->errCode & 0x10)
        return false;

    uint64 faultAddress;
    __asm__ volatile("mov %%cr2, %0" : "=r"(faultAddress));
    bool forWrite = (regs->errCode & 0x2) != 0;
    return mmapFaultHandler(currentThread(), faultAddress, forWrite);
}

uint64 isrDispatch(Registers* regs)
{
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
