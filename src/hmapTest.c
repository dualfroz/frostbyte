#include "ktest.h"
#include "hmap.h"

KTEST(hmapSetGetOverwrite)
{
    Hmap h;
    KT_TRUE(t, hmapInit(&h, 8));
    KT_EQ(t, hmapCount(&h), 0);

    int a = 1, b = 2;
    KT_TRUE(t, hmapSet(&h, "alpha", &a));
    KT_TRUE(t, hmapSet(&h, "beta", &b));
    KT_EQ(t, hmapCount(&h), 2);

    KT_EQP(t, hmapGet(&h, "alpha"), &a);
    KT_EQP(t, hmapGet(&h, "beta"), &b);
    KT_NULL(t, hmapGet(&h, "gamma"));
    KT_TRUE(t, hmapHas(&h, "alpha"));
    KT_FALSE(t, hmapHas(&h, "gamma"));

    int c = 3;
    KT_TRUE(t, hmapSet(&h, "alpha", &c));
    KT_EQ(t, hmapCount(&h), 2);
    KT_EQP(t, hmapGet(&h, "alpha"), &c);

    hmapFree(&h);
}

KTEST(hmapRemove)
{
    Hmap h;
    hmapInit(&h, 4);

    int v = 42;
    hmapSet(&h, "key", &v);
    KT_TRUE(t, hmapRemove(&h, "key"));
    KT_EQ(t, hmapCount(&h), 0);
    KT_NULL(t, hmapGet(&h, "key"));
    KT_FALSE(t, hmapRemove(&h, "key"));

    hmapFree(&h);
}

KTEST(hmapCollisionsChain)
{
    Hmap h;
    hmapInit(&h, 1);

    int values[10];
    char keys[10][8];
    for (int i = 0; i < 10; i++) {
        values[i] = i * 10;
        keys[i][0] = 'k';
        keys[i][1] = (char)('0' + i);
        keys[i][2] = 0;
        KT_TRUE(t, hmapSet(&h, keys[i], &values[i]));
    }
    KT_EQ(t, hmapCount(&h), 10);

    for (int i = 0; i < 10; i++)
        KT_EQP(t, hmapGet(&h, keys[i]), &values[i]);

    hmapFree(&h);
}

KTEST(hmapHashIsDeterministicAndDistributes)
{
    KT_EQU(t, hmapHashString("same"), hmapHashString("same"));
    KT_NE(t, hmapHashString("alpha"), hmapHashString("beta"));
    KT_NE(t, hmapHashString(""), hmapHashString("a"));
}

static bool sumVisitor(const char* key, void* value, void* userData)
{
    (void)key;
    int64* sum = (int64*)userData;
    *sum += *(int*)value;
    return true;
}

KTEST(hmapForEachVisitsAll)
{
    Hmap h;
    hmapInit(&h, 4);

    int a = 1, b = 2, c = 3;
    hmapSet(&h, "a", &a);
    hmapSet(&h, "b", &b);
    hmapSet(&h, "c", &c);

    int64 sum = 0;
    hmapForEach(&h, sumVisitor, &sum);
    KT_EQ(t, sum, 6);

    hmapFree(&h);
}
