#include "syscall.h"

.section .text
.code64

.global sigTrampolineStart
.global sigTrampolineEnd

sigTrampolineStart:
    call *%rax
    movl $sysSigReturn, %eax
    int $0x80

sigTrampolineHang:
    hlt
    jmp sigTrampolineHang

sigTrampolineEnd:

.section .note.GNU-stack, "", @progbits
