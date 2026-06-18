#ifndef FROSTBYTE_POLL_H
#define FROSTBYTE_POLL_H

#include "types.h"

/*
 * Real poll(2): waits for one or more file descriptors to become ready
 * without picking, ahead of time, which single one to block on the way
 * userRead/userWrite's own retry-the-syscall path already does for exactly
 * one fd at a time (vfs.h's vfsBlockingChannel). Bit values match real
 * Linux <poll.h> (POLLIN=0x1, POLLOUT=0x4, POLLERR=0x8, POLLHUP=0x10,
 * POLLNVAL=0x20) even though nothing here reads a Linux binary's struct --
 * there is no reason to invent different numbers for the same concept.
 */
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

/* Bounded the same way maxOpenFiles (vfs.h) bounds one process's own fd
   table -- sysPoll (syscall.c) copies the caller's array into a fixed
   kernel-stack buffer of exactly this size, so a request for more entries
   than any one process could ever have open fds for is simply rejected
   rather than silently truncated. */
#define maxPollFds 16

#endif
