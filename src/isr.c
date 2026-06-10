#include "isr.h"
#include "console.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "sched.h"
#include "ports.h"

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

uint64 isrDispatch(Registers* regs)
{
    if (regs->intNo < 32)
        panicOnException(regs);

    if (regs->intNo == 128)
        return syscallDispatch(regs);

    uint8 irq = (uint8)(regs->intNo - 32);
    uint64 nextRsp = (uint64)regs;

    if (irq == 0) {
        pitTick();
        nextRsp = schedule((uint64)regs);
    } else if (irq == 1) {
        keyboardHandler();
    }

    picSendEoi(irq);
    return nextRsp;
}
