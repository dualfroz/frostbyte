#ifndef FROSTBYTE_POLL_H
#define FROSTBYTE_POLL_H

#include "types.h"

#define pollIn   0x1
#define pollOut  0x4
#define pollErr  0x8
#define pollHup  0x10
#define pollNval 0x20

typedef struct PollFd {
    int32 fd;
    int16 events;
    int16 revents;
} PollFd;

#define maxPollFds 16

#endif
