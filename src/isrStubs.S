.section .text
.code64
.extern isrDispatch

.macro isrNoError num
.global isrStub\num
isrStub\num:
    pushq $0
    pushq $\num
    jmp isrCommon
.endm

.macro isrWithError num
.global isrStub\num
isrStub\num:
    pushq $\num
    jmp isrCommon
.endm

isrNoError 0
isrNoError 1
isrNoError 2
isrNoError 3
isrNoError 4
isrNoError 5
isrNoError 6
isrNoError 7
isrWithError 8
isrNoError 9
isrWithError 10
isrWithError 11
isrWithError 12
isrWithError 13
isrWithError 14
isrNoError 15
isrNoError 16
isrWithError 17
isrNoError 18
isrNoError 19
isrNoError 20
isrWithError 21
isrNoError 22
isrNoError 23
isrNoError 24
isrNoError 25
isrNoError 26
isrNoError 27
isrNoError 28
isrNoError 29
isrNoError 30
isrNoError 31

isrNoError 32
isrNoError 33
isrNoError 34
isrNoError 35
isrNoError 36
isrNoError 37
isrNoError 38
isrNoError 39
isrNoError 40
isrNoError 41
isrNoError 42
isrNoError 43
isrNoError 44
isrNoError 45
isrNoError 46
isrNoError 47

.global isrStub128
isrStub128:
    pushq $0
    pushq $128
    jmp isrCommon

isrCommon:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    movq %rsp, %rdi
    call isrDispatch
    movq %rax, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp
    iretq

.section .rodata
.align 8
.global isrStubTable
isrStubTable:
    .quad isrStub0
    .quad isrStub1
    .quad isrStub2
    .quad isrStub3
    .quad isrStub4
    .quad isrStub5
    .quad isrStub6
    .quad isrStub7
    .quad isrStub8
    .quad isrStub9
    .quad isrStub10
    .quad isrStub11
    .quad isrStub12
    .quad isrStub13
    .quad isrStub14
    .quad isrStub15
    .quad isrStub16
    .quad isrStub17
    .quad isrStub18
    .quad isrStub19
    .quad isrStub20
    .quad isrStub21
    .quad isrStub22
    .quad isrStub23
    .quad isrStub24
    .quad isrStub25
    .quad isrStub26
    .quad isrStub27
    .quad isrStub28
    .quad isrStub29
    .quad isrStub30
    .quad isrStub31
    .quad isrStub32
    .quad isrStub33
    .quad isrStub34
    .quad isrStub35
    .quad isrStub36
    .quad isrStub37
    .quad isrStub38
    .quad isrStub39
    .quad isrStub40
    .quad isrStub41
    .quad isrStub42
    .quad isrStub43
    .quad isrStub44
    .quad isrStub45
    .quad isrStub46
    .quad isrStub47

.section .note.GNU-stack, "", @progbits
