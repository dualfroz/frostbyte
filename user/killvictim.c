#include "ulib.h"

/*
 * Only ever spawned by killtest.c. Attaches as the READ end of a shared
 * pipe (userPipeCreate/userPipeAttach) that no writer will ever attach to,
 * so the userRead call below can only ever return by being killed --
 * proving threadKill (sched.c) genuinely recovers a process that would
 * otherwise be stuck forever now that sysRead can really block (see
 * pipe.h/vfs.h's shared-pipe machinery, added the same wave as this).
 */

int main(void)
{
    print("killvictim: start, pid=");
    printDec(userGetpid());
    print("\n");

    long handle = userPipeCreate();
    if (handle < 0) {
        print("killvictim: FAIL pipe create\n");
        return 1;
    }

    long fd = userPipeAttach(handle, 0);
    if (fd < 0) {
        print("killvictim: FAIL attach\n");
        return 1;
    }

    print("killvictim: blocking on read forever\n");
    char buffer[4];
    userRead((int)fd, buffer, sizeof(buffer));

    /* Only reached if userRead somehow returned on its own -- killtest.c
       never writes to this pipe, so that would itself be a bug. */
    print("killvictim: FAIL read returned without being killed\n");
    return 1;
}
