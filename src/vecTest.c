#include "ktest.h"
#include "vec.h"

KTEST(vecPushPopGrows)
{
    Vec v;
    vecInit(&v, sizeof(int));
    KT_TRUE(t, vecIsEmpty(&v));
    KT_EQ(t, vecCap(&v), 0);

    for (int i = 0; i < 20; i++)
        KT_TRUE(t, vecPush(&v, &i));
    KT_EQ(t, vecLen(&v), 20);
    KT_GE(t, vecCap(&v), 20);

    for (int i = 0; i < 20; i++)
        KT_EQ(t, *(int*)vecAt(&v, (usize)i), i);

    int popped = -1;
    KT_TRUE(t, vecPop(&v, &popped));
    KT_EQ(t, popped, 19);
    KT_EQ(t, vecLen(&v), 19);

    vecFree(&v);
}

KTEST(vecPopEmptyFails)
{
    Vec v;
    vecInit(&v, sizeof(int));
    int out;
    KT_FALSE(t, vecPop(&v, &out));
    KT_NULL(t, vecAt(&v, 0));
    vecFree(&v);
}

KTEST(vecSetAt)
{
    Vec v;
    vecInit(&v, sizeof(int));
    for (int i = 0; i < 5; i++)
        vecPush(&v, &i);

    int replacement = 99;
    KT_TRUE(t, vecSet(&v, 2, &replacement));
    KT_EQ(t, *(int*)vecAt(&v, 2), 99);
    KT_FALSE(t, vecSet(&v, 10, &replacement));   /* out of range */

    vecFree(&v);
}

KTEST(vecInsertRemoveShifts)
{
    Vec v;
    vecInit(&v, sizeof(int));
    for (int i = 0; i < 5; i++)
        vecPush(&v, &i);   /* 0 1 2 3 4 */

    int inserted = 100;
    KT_TRUE(t, vecInsert(&v, 2, &inserted));   /* 0 1 100 2 3 4 */
    KT_EQ(t, vecLen(&v), 6);
    KT_EQ(t, *(int*)vecAt(&v, 2), 100);
    KT_EQ(t, *(int*)vecAt(&v, 3), 2);
    KT_EQ(t, *(int*)vecAt(&v, 5), 4);

    int removed = -1;
    KT_TRUE(t, vecRemove(&v, 2, &removed));    /* back to 0 1 2 3 4 */
    KT_EQ(t, removed, 100);
    KT_EQ(t, vecLen(&v), 5);
    for (int i = 0; i < 5; i++)
        KT_EQ(t, *(int*)vecAt(&v, (usize)i), i);

    vecFree(&v);
}

KTEST(vecClearKeepsCapacity)
{
    Vec v;
    vecInit(&v, sizeof(int));
    for (int i = 0; i < 8; i++)
        vecPush(&v, &i);
    usize capBefore = vecCap(&v);

    vecClear(&v);
    KT_EQ(t, vecLen(&v), 0);
    KT_TRUE(t, vecIsEmpty(&v));
    KT_EQ(t, vecCap(&v), capBefore);   /* clear does not free backing storage */

    int again = 7;
    KT_TRUE(t, vecPush(&v, &again));
    KT_EQ(t, *(int*)vecAt(&v, 0), 7);

    vecFree(&v);
}

KTEST(vecStructElements)
{
    typedef struct { int32 x; int32 y; } Point;

    Vec v;
    vecInit(&v, sizeof(Point));

    Point a = { 1, 2 };
    Point b = { 3, 4 };
    vecPush(&v, &a);
    vecPush(&v, &b);

    Point* pa = (Point*)vecAt(&v, 0);
    Point* pb = (Point*)vecAt(&v, 1);
    KT_EQ(t, pa->x, 1);
    KT_EQ(t, pa->y, 2);
    KT_EQ(t, pb->x, 3);
    KT_EQ(t, pb->y, 4);

    vecFree(&v);
}
