#include "ulib.h"
#include "fs.h"

int main(void)
{
    print("dirpermtest: start, pid=");
    printDec(userGetpid());
    print("\n");

    if (userGetuid() != 0) {
        print("dirpermtest: FAIL boot-time uid is not 0\n");
        return 1;
    }

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

    if (userMkdir("/tmp/dpd/writable") != 0) {
        print("dirpermtest: FAIL mkdir writable\n");
        return 1;
    }
    if (userChmod("/tmp/dpd/writable", 0777) != 0) {
        print("dirpermtest: FAIL chmod writable to 0777\n");
        return 1;
    }

    if (userMkdir("/tmp/dpd/owned") != 0) {
        print("dirpermtest: FAIL mkdir owned\n");
        return 1;
    }
    if (userChown("/tmp/dpd/owned", 1000) != 0) {
        print("dirpermtest: FAIL chown owned to 1000\n");
        return 1;
    }

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

    if (userSetuid(1000) != 0 || userGetuid() != 1000) {
        print("dirpermtest: FAIL setuid(1000)\n");
        return 1;
    }
    print("dirpermtest: drop-to-1000-ok\n");

    if (userOpen("/tmp/dpd/newfile.txt", oWriteOnly | oCreate) >= 0) {
        print("dirpermtest: FAIL non-owner created entry in default-mode dir\n");
        return 1;
    }
    print("dirpermtest: parent-write-denied-ok\n");

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

    if (userOpen("/tmp/dpd/noexec/inside.txt", oReadOnly) >= 0) {
        print("dirpermtest: FAIL traversed into a no-exec directory\n");
        return 1;
    }
    print("dirpermtest: traverse-denied-ok\n");

    char buf[4];
    int rfd = (int)userOpen("/tmp/dpd/shared.txt", oReadOnly);
    if (rfd < 0 || userRead(rfd, buf, 1) != 1) {
        print("dirpermtest: FAIL could not read shared.txt through a searchable dir\n");
        return 1;
    }
    userClose(rfd);
    print("dirpermtest: traverse-allowed-ok\n");

    if (userOpen("/tmp/dpd/noread", oReadOnly) >= 0) {
        print("dirpermtest: FAIL listed a directory with no read bit\n");
        return 1;
    }
    print("dirpermtest: list-denied-ok\n");

    int nrfd = (int)userOpen("/tmp/dpd/noread/visible.txt", oReadOnly);
    if (nrfd < 0) {
        print("dirpermtest: FAIL could not open known file via exec-only dir\n");
        return 1;
    }
    userClose(nrfd);
    print("dirpermtest: search-without-list-ok\n");

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
