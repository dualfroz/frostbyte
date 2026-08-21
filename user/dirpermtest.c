#include "ulib.h"
#include "fs.h"

/*
 * Real ring-3 proof of directory-level permission enforcement (vfs.c's
 * lookupNode/vfsMkdir/resolveOpen, sfs.c's lookupFrom/createNode/sfsRemove/
 * sfsLink) -- the gap FALA 44 explicitly flagged as stored-but-unenforced:
 * before this fala, a directory's own mode bits were reported by stat/ls -l
 * but NEVER consulted by anything, so ANY uid could create/remove entries
 * in ANY directory and traverse ANY directory tree regardless of its mode.
 * Fully synchronous like identtest.c/permtest.c/rmtest.c: no injected
 * keystrokes, no shared shell channel, so there is no scheduling-timing
 * race to flake on regardless of host load.
 *
 * Three distinct real POSIX bits are proven separately, not just "denied
 * vs allowed" as one blob:
 *   - write on a PARENT directory governs creating/removing entries in it
 *     (not the entry's own mode -- real unlink(2)'s own rule).
 *   - exec (search) on every ANCESTOR directory governs resolving a NAME
 *     inside it, checked on every directory a path walks through.
 *   - read on a directory ITSELF governs listing its entries (getdents),
 *     a SEPARATE bit from exec -- a directory can be searchable (you can
 *     open a file inside it by name) without being listable, and this test
 *     proves that distinction concretely, not just asserts it in a comment.
 */

int main(void)
{
    print("dirpermtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userGetuid() != 0) {
        print("dirpermtest: FAIL boot-time uid is not 0\n");
        return 1;
    }

    /* --- setup, entirely as root --- */
    if (userMkdir("/tmp/dpd") != 0) {
        print("dirpermtest: FAIL mkdir /tmp/dpd\n");
        return 1;
    }
    int sfd = (int)userOpen("/tmp/dpd/shared.txt", oReadWrite | oCreate);
    if (sfd < 0 || userWrite(sfd, "s", 1) != 1) {
        print("dirpermtest: FAIL create shared.txt\n");
        return 1;
    }
    userClose(sfd);

    /* noexec: root removes ALL bits (0000) after creating one file inside
       it -- no owner or other exec at all, so NOBODY (not even root) can
       search inside it afterward. */
    if (userMkdir("/tmp/dpd/noexec") != 0) {
        print("dirpermtest: FAIL mkdir noexec\n");
        return 1;
    }
    int ifd = (int)userOpen("/tmp/dpd/noexec/inside.txt", oReadWrite | oCreate);
    if (ifd < 0 || userWrite(ifd, "i", 1) != 1) {
        print("dirpermtest: FAIL create inside.txt\n");
        return 1;
    }
    userClose(ifd);
    if (userChmod("/tmp/dpd/noexec", 0000) != 0) {
        print("dirpermtest: FAIL chmod noexec to 0000\n");
        return 1;
    }

    /* noread: owner keeps full rwx, other gets search (x) but not list (r)
       -- proves exec and read are genuinely separate bits, not aliases. */
    if (userMkdir("/tmp/dpd/noread") != 0) {
        print("dirpermtest: FAIL mkdir noread\n");
        return 1;
    }
    int vfd = (int)userOpen("/tmp/dpd/noread/visible.txt", oReadWrite | oCreate);
    if (vfd < 0 || userWrite(vfd, "v", 1) != 1) {
        print("dirpermtest: FAIL create visible.txt\n");
        return 1;
    }
    userClose(vfd);
    if (userChmod("/tmp/dpd/noread", 0711) != 0) {
        print("dirpermtest: FAIL chmod noread to 0711\n");
        return 1;
    }

    /* writable: full rwx for owner AND other -- proves a non-owner CAN
       create/remove entries when the other-write bit genuinely allows it,
       not that this feature is a blanket deny. */
    if (userMkdir("/tmp/dpd/writable") != 0) {
        print("dirpermtest: FAIL mkdir writable\n");
        return 1;
    }
    if (userChmod("/tmp/dpd/writable", 0777) != 0) {
        print("dirpermtest: FAIL chmod writable to 0777\n");
        return 1;
    }

    /* owned: reassigned to uid 1000 (real chown(2)) BEFORE this process
       ever drops privilege, mirroring identtest.c's owned.txt -- after the
       drop, this process really is the OWNER, so the owner bits (not the
       other bits) govern. */
    if (userMkdir("/tmp/dpd/owned") != 0) {
        print("dirpermtest: FAIL mkdir owned\n");
        return 1;
    }
    if (userChown("/tmp/dpd/owned", 1000) != 0) {
        print("dirpermtest: FAIL chown owned to 1000\n");
        return 1;
    }

    /* rootRO: root removes its OWN write bit (0555) -- proves no uid-0
       superuser bypass exists for directory writes either, matching
       vfsPermitted's own file-level rule (identtest.c's chmod-nonowner-
       denied-ok proved the same "no bypass" story for file bits). Checked
       WHILE STILL ROOT, before the drop below. */
    if (userMkdir("/tmp/dpd/rootRO") != 0) {
        print("dirpermtest: FAIL mkdir rootRO\n");
        return 1;
    }
    if (userChmod("/tmp/dpd/rootRO", 0555) != 0) {
        print("dirpermtest: FAIL chmod rootRO to 0555\n");
        return 1;
    }
    print("dirpermtest: setup-ok\n");

    if (userOpen("/tmp/dpd/rootRO/x", oWriteOnly | oCreate) >= 0) {
        print("dirpermtest: FAIL root created a file with its own write bit off\n");
        return 1;
    }
    print("dirpermtest: root-self-write-denied-ok\n");

    /* --- real, permanent privilege drop --- */
    if (userSetuid(1000) != 0 || userGetuid() != 1000) {
        print("dirpermtest: FAIL setuid(1000)\n");
        return 1;
    }
    print("dirpermtest: drop-to-1000-ok\n");

    /* /tmp/dpd itself is root-owned, default 0755 (other = r-x, no w) --
       a non-owner cannot create a new entry in it. */
    if (userOpen("/tmp/dpd/newfile.txt", oWriteOnly | oCreate) >= 0) {
        print("dirpermtest: FAIL non-owner created entry in default-mode dir\n");
        return 1;
    }
    print("dirpermtest: parent-write-denied-ok\n");

    /* writable/ is 0777 -- other-write is genuinely granted, so create AND
       remove must both succeed for this non-owner. */
    int nfd = (int)userOpen("/tmp/dpd/writable/n.txt", oWriteOnly | oCreate);
    if (nfd < 0) {
        print("dirpermtest: FAIL non-owner denied create in 0777 dir\n");
        return 1;
    }
    userClose(nfd);
    print("dirpermtest: other-write-allowed-ok\n");
    if (userUnlink("/tmp/dpd/writable/n.txt") != 0) {
        print("dirpermtest: FAIL non-owner denied unlink in 0777 dir\n");
        return 1;
    }
    print("dirpermtest: other-unlink-allowed-ok\n");

    /* noexec/ has NO exec bit for anyone -- resolving inside.txt inside it
       must fail at the ancestor search-permission check, not at some later
       stage. */
    if (userOpen("/tmp/dpd/noexec/inside.txt", oReadOnly) >= 0) {
        print("dirpermtest: FAIL traversed into a no-exec directory\n");
        return 1;
    }
    print("dirpermtest: traverse-denied-ok\n");

    /* /tmp/dpd itself is still searchable (other-exec=1 from the default
       0755) even though this same process cannot WRITE into it -- proves
       the denial above was specific to noexec's own missing bit, not a
       blanket failure of every directory this non-owner touches. */
    char buf[4];
    int rfd = (int)userOpen("/tmp/dpd/shared.txt", oReadOnly);
    if (rfd < 0 || userRead(rfd, buf, 1) != 1) {
        print("dirpermtest: FAIL could not read shared.txt through a searchable dir\n");
        return 1;
    }
    userClose(rfd);
    print("dirpermtest: traverse-allowed-ok\n");

    /* noread/ grants other-exec but not other-read: opening the DIRECTORY
       itself for listing must fail... */
    if (userOpen("/tmp/dpd/noread", oReadOnly) >= 0) {
        print("dirpermtest: FAIL listed a directory with no read bit\n");
        return 1;
    }
    print("dirpermtest: list-denied-ok\n");

    /* ...but opening a FILE inside it BY NAME must still succeed, since
       that only needs search (exec), never list (read), on noread itself
       -- the exact distinction real Unix's `ls somedir/knownfile` (works
       without +r on somedir) vs `ls somedir` (needs +r) draws. */
    int nrfd = (int)userOpen("/tmp/dpd/noread/visible.txt", oReadOnly);
    if (nrfd < 0) {
        print("dirpermtest: FAIL could not open known file via exec-only dir\n");
        return 1;
    }
    userClose(nrfd);
    print("dirpermtest: search-without-list-ok\n");

    /* owned/ was chowned to 1000 before the drop -- this process really is
       the owner now, so default 0755's OWNER bits (full rwx) govern, not
       the other bits a stranger would be limited to. */
    int ofd = (int)userOpen("/tmp/dpd/owned/mine.txt", oWriteOnly | oCreate);
    if (ofd < 0) {
        print("dirpermtest: FAIL owner denied create in its own directory\n");
        return 1;
    }
    userClose(ofd);
    if (userUnlink("/tmp/dpd/owned/mine.txt") != 0) {
        print("dirpermtest: FAIL owner denied unlink in its own directory\n");
        return 1;
    }
    print("dirpermtest: owner-write-allowed-ok\n");

    print("dirpermtest: PASS\n");
    return 0;
}
