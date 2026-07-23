#ifndef FROSTBYTE_KTEST_H
#define FROSTBYTE_KTEST_H

#include "types.h"

/*
 * In-kernel self-test harness (KUnit-style).
 *
 * A test is written as:
 *
 *     #include "ktest.h"
 *
 *     KTEST(mySubsystemBasics)
 *     {
 *         KT_EQ(t, 2 + 2, 4);
 *         KT_TRUE(t, somePredicate());
 *     }
 *
 * The KTEST macro both declares the test body and registers a pointer to it
 * into the ".ktests" linker section, so no shared registry file has to be
 * edited when a new test is added.  ktestRunAll() (called once from kmain
 * before the scheduler starts) iterates the section, runs every test, prints
 * per-test results and a final machine-readable line:
 *
 *     KTEST SUMMARY: <tests> tests, <checks> checks, <failures> failed
 *
 * The boot gate (tools/ci-test.sh) requires that line to report 0 failures,
 * so a broken subsystem makes the gate fail instead of silently passing.
 *
 * Tests run with interrupts disabled, before the scheduler starts.  They must
 * be deterministic and must NOT depend on timer ticks, preemption or user
 * threads.  They may use kmalloc/kfree, the string helpers, pmm and any
 * subsystem they explicitly initialise themselves.
 */

typedef struct KtestCtx {
    const char* name;
    uint64 checks;
    uint64 failures;
} KtestCtx;

typedef void (*KtestFn)(KtestCtx* t);

typedef struct KtestEntry {
    const char* name;
    KtestFn fn;
} KtestEntry;

/* Runs every registered test, prints results, returns the number of failed
   checks across all tests (0 means the whole battery is green). */
uint64 ktestRunAll(void);

/* Records one check.  On failure it prints "FAIL file:line  expr" and marks
   the current test context as failed.  Returns the boolean result so callers
   can branch (e.g. skip a dereference after a null check failed). */
bool ktestCheck(KtestCtx* t, bool ok, const char* expr, const char* file, int line);

/* Null-safe string comparison used by KT_STREQ. */
bool ktestStrEq(const char* a, const char* b);

#define KTEST(testName)                                                          \
    static void testName##KtestFn(KtestCtx* t);                                  \
    static const KtestEntry testName##KtestEntry = { #testName, testName##KtestFn }; \
    __attribute__((section(".ktests"), used))                                    \
    static const KtestEntry* const testName##KtestPtr = &testName##KtestEntry;   \
    static void testName##KtestFn(KtestCtx* t)

#define KT_TRUE(t, cond)   ktestCheck((t), (cond) ? true : false, #cond, __FILE__, __LINE__)
#define KT_FALSE(t, cond)  ktestCheck((t), (cond) ? false : true, "!(" #cond ")", __FILE__, __LINE__)
#define KT_EQ(t, a, b)     ktestCheck((t), (int64)(a) == (int64)(b), #a " == " #b, __FILE__, __LINE__)
#define KT_NE(t, a, b)     ktestCheck((t), (int64)(a) != (int64)(b), #a " != " #b, __FILE__, __LINE__)
#define KT_LT(t, a, b)     ktestCheck((t), (int64)(a) <  (int64)(b), #a " < "  #b, __FILE__, __LINE__)
#define KT_LE(t, a, b)     ktestCheck((t), (int64)(a) <= (int64)(b), #a " <= " #b, __FILE__, __LINE__)
#define KT_GT(t, a, b)     ktestCheck((t), (int64)(a) >  (int64)(b), #a " > "  #b, __FILE__, __LINE__)
#define KT_GE(t, a, b)     ktestCheck((t), (int64)(a) >= (int64)(b), #a " >= " #b, __FILE__, __LINE__)
#define KT_EQU(t, a, b)    ktestCheck((t), (uint64)(a) == (uint64)(b), #a " ==u " #b, __FILE__, __LINE__)
#define KT_EQP(t, a, b)    ktestCheck((t), (const void*)(a) == (const void*)(b), #a " ==p " #b, __FILE__, __LINE__)
#define KT_NULL(t, p)      ktestCheck((t), (const void*)(p) == nullptr, #p " == null", __FILE__, __LINE__)
#define KT_NOTNULL(t, p)   ktestCheck((t), (const void*)(p) != nullptr, #p " != null", __FILE__, __LINE__)
#define KT_STREQ(t, a, b)  ktestCheck((t), ktestStrEq((a), (b)), #a " streq " #b, __FILE__, __LINE__)

#endif
