#include "ktest.h"
#include "bmp.h"

KTEST(bmpSetClearTest)
{
    uint64 words[2];
    Bitmap b;
    bmpInit(&b, words, 100);

    KT_FALSE(t, bmpTest(&b, 0));
    KT_FALSE(t, bmpTest(&b, 63));
    KT_FALSE(t, bmpTest(&b, 64));
    KT_FALSE(t, bmpTest(&b, 99));

    bmpSet(&b, 0);
    bmpSet(&b, 63);
    bmpSet(&b, 64);
    bmpSet(&b, 99);
    KT_TRUE(t, bmpTest(&b, 0));
    KT_TRUE(t, bmpTest(&b, 63));
    KT_TRUE(t, bmpTest(&b, 64));
    KT_TRUE(t, bmpTest(&b, 99));
    KT_FALSE(t, bmpTest(&b, 1));
    KT_EQ(t, bmpCountSet(&b), 4);

    bmpClear(&b, 63);
    KT_FALSE(t, bmpTest(&b, 63));
    KT_EQ(t, bmpCountSet(&b), 3);
}

KTEST(bmpOutOfRangeIsSafeNoop)
{
    uint64 words[1];
    Bitmap b;
    bmpInit(&b, words, 10);

    bmpSet(&b, 500);
    KT_FALSE(t, bmpTest(&b, 500));
    bmpClear(&b, 500);
    KT_EQ(t, bmpCountSet(&b), 0);
}

KTEST(bmpRangeOps)
{
    uint64 words[1];
    Bitmap b;
    bmpInit(&b, words, 20);

    bmpSetRange(&b, 5, 10);
    KT_EQ(t, bmpCountSet(&b), 10);
    KT_FALSE(t, bmpTest(&b, 4));
    KT_TRUE(t, bmpTest(&b, 5));
    KT_TRUE(t, bmpTest(&b, 14));
    KT_FALSE(t, bmpTest(&b, 15));

    bmpClearRange(&b, 5, 3);
    KT_FALSE(t, bmpTest(&b, 5));
    KT_FALSE(t, bmpTest(&b, 7));
    KT_TRUE(t, bmpTest(&b, 8));
    KT_EQ(t, bmpCountSet(&b), 7);
}

KTEST(bmpSetAllClearAllAndFind)
{
    uint64 words[2];
    Bitmap b;
    bmpInit(&b, words, 70);

    KT_EQ(t, bmpFindFirstSet(&b), -1);
    KT_EQ(t, bmpFindFirstClear(&b), 0);

    bmpSet(&b, 5);
    KT_EQ(t, bmpFindFirstSet(&b), 5);

    bmpSetAll(&b);
    KT_EQ(t, bmpCountSet(&b), 70);
    KT_EQ(t, bmpFindFirstClear(&b), -1);
    KT_EQ(t, bmpFindFirstSet(&b), 0);

    bmpClear(&b, 30);
    KT_EQ(t, bmpFindFirstClear(&b), 30);

    bmpClearAll(&b);
    KT_EQ(t, bmpCountSet(&b), 0);
    KT_EQ(t, bmpFindFirstSet(&b), -1);
}
