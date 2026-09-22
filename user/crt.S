.section .text
.global _start
_start:
    xorq %rbp, %rbp
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    call main
    movq %rax, %rdi
    movq $0, %rax
    int $0x80
1:
    jmp 1b

.section .note.GNU-stack, "", @progbits
