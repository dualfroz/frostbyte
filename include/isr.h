#ifndef FROSTBYTE_ISR_H
#define FROSTBYTE_ISR_H

#include "types.h"

typedef struct Registers {
    uint64 r15, r14, r13, r12, r11, r10, r9, r8;
    uint64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64 intNo, errCode;
    uint64 rip, cs, rflags, rsp, ss;
} __attribute__((packed)) Registers;

uint64 isrDispatch(Registers* regs);
uint64 syscallDispatch(Registers* regs);

#endif
