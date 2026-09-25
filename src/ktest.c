#include "ktest.h"
#include "console.h"

extern const KtestEntry* const ktestArrayStart[];
extern const KtestEntry* const ktestArrayEnd[];

bool ktestStrEq(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr)
        return a == b;
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return *a == *b;
}

bool ktestCheck(KtestCtx* t, bool ok, const char* expr, const char* file, int line)
{
    t->checks++;
    if (!ok) {
        t->failures++;
        kprintf("    FAIL %s:%d  %s\n", file, line, expr);
    }
    return ok;
}

uint64 ktestRunAll(void)
{
    uint64 totalTests = 0;
    uint64 totalChecks = 0;
    uint64 totalFailures = 0;
    uint64 failedTests = 0;

    kprintf("\n==== ktest: running in-kernel self-tests ====\n");

    for (const KtestEntry* const* p = ktestArrayStart; p < ktestArrayEnd; p++) {
        const KtestEntry* e = *p;
        KtestCtx t = { e->name, 0, 0 };
        e->fn(&t);
        totalTests++;
        totalChecks += t.checks;
        totalFailures += t.failures;
        if (t.failures) {
            failedTests++;
            kprintf("  [FAIL] %s (%lu/%lu checks failed)\n", e->name, t.failures, t.checks);
        } else {
            kprintf("  [ ok ] %s (%lu checks)\n", e->name, t.checks);
        }
    }

    kprintf("==== ktest done: %lu of %lu tests failed ====\n", failedTests, totalTests);
    kprintf("KTEST SUMMARY: %lu tests, %lu checks, %lu failed\n",
            totalTests, totalChecks, totalFailures);
    return totalFailures;
}
