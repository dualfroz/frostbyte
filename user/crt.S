.section .text
.global _start
_start:
    xorq %rbp, %rbp
    /* Real x86_64 SysV process entry: rsp points at argc, with argv[0..]
       and a NULL terminator right above it (elfBuildInitialStack,
       elf.c/syscall.c doExec/doSpawn, kmain.c spawnUser). */
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    call main
    movq %rax, %rdi
    movq $0, %rax
    int $0x80
1:
    jmp 1b

.section .note.GNU-stack, "", @progbits
