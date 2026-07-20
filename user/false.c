#include "ulib.h"

/*
 * Real POSIX false(1): does nothing, always exits 1. Exists purely so
 * shell.c's new `&&`/`||` conditional chaining (execute()/executeSingle,
 * shellSplitAndOr in shell.h) has a genuinely SPAWNED process (real
 * fork-less spawn + real userExit, not just a builtin's own int return
 * like doExport/changeDir) whose nonzero exit status drives a real `&&`
 * skip followed by a real `||` fallback -- kmain.c's andOrDriverThread
 * types `false && echo x || echo ao2` at the live shell to prove it.
 */
int main(void)
{
    return 1;
}
