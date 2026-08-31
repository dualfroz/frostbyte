#include "ulib.h"
#include "fs.h"

/*
 * Real ring-3 proof of unlink(2)/rmdir(2) as two DISTINCT syscalls
 * (sysUnlink=13, pre-existing; sysRmdir=43, new this fala) through
 * userUnlink/userRmdir (ulib.h), exercising both tmpfs (/tmp) and the
 * persistent on-disk SFS mount (/mnt) to prove vfs.c's dispatch layer
 * enforces the same real POSIX split -- unlink refuses a directory,
 * rmdir refuses anything that is not an EMPTY directory -- on both
 * backing stores, not just one. Fully synchronous like nicetest.c/
 * date.c/timetest.c: no injected keystrokes, no shared shell channel, so
 * there is no scheduling-timing race to flake on regardless of host load.
 */

static int createFile(const char* path)
{
    int fd = (int)userOpen(path, oReadWrite | oCreate);
    if (fd < 0)
        return -1;
    userClose(fd);
    return 0;
}

int main(void)
{
    print("rmtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userMkdir("/tmp/rmtest") != 0) {
        print("rmtest: FAIL mkdir /tmp/rmtest\n");
        return 1;
    }
    if (createFile("/tmp/rmtest/f.txt") != 0) {
        print("rmtest: FAIL create /tmp/rmtest/f.txt\n");
        return 1;
    }

    if (userRmdir("/tmp/rmtest") == 0) {
        print("rmtest: FAIL rmdir succeeded on a non-empty tmpfs dir\n");
        return 1;
    }
    if (userUnlink("/tmp/rmtest") == 0) {
        print("rmtest: FAIL unlink succeeded on a tmpfs directory\n");
        return 1;
    }
    FbStat st;
    if (userStat("/tmp/rmtest", &st) != 0 || st.type != fsTypeDir) {
        print("rmtest: FAIL tmpfs dir did not survive the rejected attempts\n");
        return 1;
    }
    print("rmtest: tmpfs-reject-ok\n");

    if (userUnlink("/tmp/rmtest/f.txt") != 0) {
        print("rmtest: FAIL unlink /tmp/rmtest/f.txt\n");
        return 1;
    }
    if (userRmdir("/tmp/rmtest") != 0) {
        print("rmtest: FAIL rmdir empty tmpfs dir\n");
        return 1;
    }
    if (userStat("/tmp/rmtest", &st) == 0) {
        print("rmtest: FAIL tmpfs dir still present after rmdir\n");
        return 1;
    }
    print("rmtest: tmpfs-remove-ok\n");

    if (userMkdir("/mnt/rmtest") != 0) {
        print("rmtest: FAIL mkdir /mnt/rmtest\n");
        return 1;
    }

    /* The exact previously-shipped gap this fala closed: unlinking an
       EMPTY on-disk directory used to silently succeed (sfsRemove's own
       dir-emptiness check let it through with no type gate above it) --
       must now be refused, EISDIR-shaped, same as tmpfs above. */
    if (userUnlink("/mnt/rmtest") == 0) {
        print("rmtest: FAIL unlink succeeded on an empty SFS directory\n");
        return 1;
    }
    if (userStat("/mnt/rmtest", &st) != 0 || st.type != fsTypeDir) {
        print("rmtest: FAIL SFS dir did not survive the rejected unlink\n");
        return 1;
    }
    print("rmtest: sfs-reject-ok\n");

    if (createFile("/mnt/rmtest/g.txt") != 0) {
        print("rmtest: FAIL create /mnt/rmtest/g.txt\n");
        return 1;
    }
    if (userRmdir("/mnt/rmtest") == 0) {
        print("rmtest: FAIL rmdir succeeded on a non-empty SFS dir\n");
        return 1;
    }
    if (userUnlink("/mnt/rmtest/g.txt") != 0) {
        print("rmtest: FAIL unlink /mnt/rmtest/g.txt\n");
        return 1;
    }
    if (userRmdir("/mnt/rmtest") != 0) {
        print("rmtest: FAIL rmdir empty SFS dir\n");
        return 1;
    }
    if (userStat("/mnt/rmtest", &st) == 0) {
        print("rmtest: FAIL SFS dir still present after rmdir\n");
        return 1;
    }
    print("rmtest: sfs-remove-ok\n");

    print("rmtest: PASS\n");
    return 0;
}
