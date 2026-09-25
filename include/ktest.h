#ifndef FROSTBYTE_KTEST_H
#define FROSTBYTE_KTEST_H

#include "types.h"

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

uint64 ktestRunAll(void);

bool ktestCheck(KtestCtx* t, bool ok, const char* expr, const char* file, int line);

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
