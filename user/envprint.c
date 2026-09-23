#include "ulib.h"

int main(void)
{
    print("envprint: start, pid=");
    printDec(userGetpid());
    print("\n");

    char value[32];
    long len = userGetEnv("GREETING", value, sizeof(value));
    if (len < 0) {
        print("envprint: FAIL missing inherited GREETING\n");
        return 1;
    }
    printParts((const char* const[]){ "envprint: inherited-ok GREETING=", value, "\n", 0 });

    if (userGetEnv("NO_SUCH_VAR", value, sizeof(value)) >= 0) {
        print("envprint: FAIL unset variable returned a value\n");
        return 1;
    }

    if (userSetEnv("GREETING", "child-mutated") != 0) {
        print("envprint: FAIL setenv\n");
        return 1;
    }
    len = userGetEnv("GREETING", value, sizeof(value));
    if (len < 0) {
        print("envprint: FAIL read back own setenv\n");
        return 1;
    }
    printParts((const char* const[]){ "envprint: self-set-ok GREETING=", value, "\n", 0 });

    if (userUnsetEnv("GREETING") != 0) {
        print("envprint: FAIL unset\n");
        return 1;
    }
    if (userGetEnv("GREETING", value, sizeof(value)) >= 0) {
        print("envprint: FAIL unset did not remove variable\n");
        return 1;
    }
    print("envprint: unset-ok\n");

    if (userUnsetEnv("NO_SUCH_VAR_EVER") != 0) {
        print("envprint: FAIL unset-missing should be a no-op success\n");
        return 1;
    }
    print("envprint: unset-missing-ok\n");

    return 0;
}
