.set multibootMagic,        0xE85250D6
.set multibootArch,         0
.set kernelVirtualBase,     0xFFFFFFFF80000000

.section .multiboot, "a"
.align 8
multibootHeaderStart:
    .long multibootMagic
    .long multibootArch
    .long multibootHeaderEnd - multibootHeaderStart
    .long -(multibootMagic + multibootArch + (multibootHeaderEnd - multibootHeaderStart))

    .align 8
    .short 0
    .short 0
    .long 8
multibootHeaderEnd:

.section .boot.text, "ax"
.code32
.global _start
_start:
    mov %eax, %edi
    mov %ebx, %esi

    call checkLongMode

    movl $bootPdptLow, %eax
    orl $0x3, %eax
    movl %eax, bootPml4 + 0

    movl $bootPdptHigh, %eax
    orl $0x3, %eax
    movl %eax, bootPml4 + 511 * 8

    movl $bootPd, %eax
    orl $0x3, %eax
    movl %eax, bootPdptLow + 0
    movl %eax, bootPdptHigh + 510 * 8

    xorl %ecx, %ecx
    movl $bootPd, %edx
fillPd:
    movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax
    movl %eax, (%edx)
    addl $8, %edx
    incl %ecx
    cmpl $512, %ecx
    jl fillPd

    movl $bootPml4, %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    movl $0xC0000080, %ecx
    rdmsr
    orl $(1 << 8), %eax
    wrmsr

    movl %cr0, %eax
    orl $((1 << 31) | (1 << 0)), %eax
    movl %eax, %cr0

    lgdt bootGdtPointer
    ljmp $0x08, $longModeEntry

checkLongMode:
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb noLongMode
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz noLongMode
    ret

noLongMode:
    cli
1:
    hlt
    jmp 1b

.code64
longModeEntry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movabs $higherHalfEntry, %rax
    jmp *%rax

.section .boot.text, "ax"
.align 16
bootGdtStart:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF
    .quad 0x00CF92000000FFFF
bootGdtEnd:

.align 8
bootGdtPointer:
    .word bootGdtEnd - bootGdtStart - 1
    .quad bootGdtStart

.section .boot.pagetables, "aw", @progbits
.align 4096
.global bootPml4
bootPml4:
    .skip 4096
bootPdptLow:
    .skip 4096
bootPdptHigh:
    .skip 4096
bootPd:
    .skip 4096

.section .text
.code64
.global higherHalfEntry
.extern kmain
higherHalfEntry:
    movabs $stackTop, %rsp
    xorq %rbp, %rbp

    movl %edi, %edi
    movl %esi, %esi
    call kmain

hang:
    cli
    hlt
    jmp hang

.section .bss
.align 16
stackBottom:
    .skip 16384
stackTop:

.section .note.GNU-stack, "", @progbits
