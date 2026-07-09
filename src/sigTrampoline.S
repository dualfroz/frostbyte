#include "syscall.h"

/*
 * Shared signal-delivery trampoline (see signal.h/signal.c). This is NOT
 * entered like a normal function call from C -- signal.c's
 * sigConsumePending rewrites a thread's own saved trapframe so that
 * iretq lands ring 3 execution directly at sigTrampolineStart, with %rax
 * already holding the handler's address and %rdi already holding the
 * signal number (the handler's sole SysV argument), exactly as if the
 * kernel itself had performed `call *%rax` with that argument set up.
 *
 * Position independent by construction: every instruction here is either
 * PC-relative (the final backward jmp) or references no address at all
 * (call through a register, an immediate syscall number, int $0x80) -- so
 * sigInstallTrampoline can copy these bytes to any physical frame and map
 * that frame at any virtual address (sigTrampolineVirtualAddr) and it
 * still runs correctly, unlike this same code linked and executed in
 * place inside the kernel's own (very different) virtual address range.
 */
.section .text
.code64

.global sigTrampolineStart
.global sigTrampolineEnd

sigTrampolineStart:
    call *%rax
    movl $sysSigReturn, %eax
    int $0x80

/* Only reached if sysSigReturn somehow returned instead of restoring the
   pre-signal context and resuming it (a kernel bug, not a state a correct
   sysOnSigReturn can produce) -- park here rather than run off into
   whatever garbage happens to follow this page. */
sigTrampolineHang:
    hlt
    jmp sigTrampolineHang

sigTrampolineEnd:

.section .note.GNU-stack, "", @progbits
