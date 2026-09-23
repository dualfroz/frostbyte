#ifndef FROSTBYTE_SYSCALL_H
#define FROSTBYTE_SYSCALL_H

#define sysExit     0
#define sysWrite    1
#define sysRead     2
#define sysYield    3
#define sysGetpid   4
#define sysSbrk     5
#define sysSleep    6
#define sysOpen     7
#define sysClose    8
#define sysLseek    9
#define sysStat     10
#define sysGetdents 11
#define sysMkdir    12
#define sysUnlink   13
#define sysPipe     14
#define sysSymlink  15
#define sysReadlink 16
#define sysLink     17
#define sysExec       18
#define sysSpawn      19
#define sysWait       20
#define sysPipeCreate 21
#define sysPipeAttach 22
#define sysKill       23
#define sysMkfifo     24
#define sysSigSend     25
#define sysSigAction   26
#define sysSigProcMask 27
#define sysSigReturn   28
#define sysAlarm       29
#define sysSetpgid     30
#define sysGetpgid     31
#define sysSetFgPgrp   32
#define sysGetFgPgrp   33
#define sysPoll        34
#define sysChdir       35
#define sysGetcwd      36
#define sysGetEnv      37
#define sysSetEnv      38
#define sysNice        39
#define sysGetPriority 40
#define sysTime        41
#define sysUnsetEnv    42
#define sysRmdir       43
#define sysChmod       44
#define sysGetuid      45
#define sysSetuid      46
#define sysChown       47
#define sysGetgid      48
#define sysSetgid      49
#define sysChgrp       50
#define sysPidfdOpen   51
#define sysPidfdSend   52
#define sysPidfdWait   53
#define sysMmap        54
#define sysMunmap      55

#define waitAnyPid ((unsigned int)-1)

#define spawnSlotBusy (-2)

#endif
