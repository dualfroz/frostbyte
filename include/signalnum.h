#ifndef FROSTBYTE_SIGNALNUM_H
#define FROSTBYTE_SIGNALNUM_H

#define sigHup   1
#define sigInt   2
#define sigQuit  3
#define sigIll   4
#define sigAbrt  6
#define sigFpe   8
#define sigKill  9
#define sigUsr1  10
#define sigSegv  11
#define sigUsr2  12
#define sigPipe  13
#define sigAlrm  14
#define sigTerm  15
#define sigChld  17
#define sigCont  18
#define sigStop  19
#define sigTstp  20

#define sigHandlerDefault 0ull
#define sigHandlerIgnore  1ull

#define sigSetFailed (~0ull)

#define sigMaskBlock    0
#define sigMaskUnblock  1
#define sigMaskSetMask  2

#define sigFlagRestart (1ull << 0)

#define sigFlagNoDefer (1ull << 1)

#define sigFlagSigInfo (1ull << 2)

#define siCodeUser          0
#define siCodeKernel        1
#define siCodeChldExited    2
#define siCodeChldKilled    3
#define siCodeChldStopped   4
#define siCodeChldContinued 5

typedef struct SigInfo {
    int siSignum;
    int siCode;
    int siPid;
    int siStatus;
} SigInfo;

#define errInterrupted (-4)

#endif
