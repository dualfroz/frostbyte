#include "ktest.h"
#include "ring.h"

KTEST(ringBasicPushPop)
{
    uint8 backing[4];
    RingBuffer r;
    ringInit(&r, backing, sizeof(backing));

    KT_TRUE(t, ringIsEmpty(&r));
    KT_FALSE(t, ringIsFull(&r));
    KT_EQ(t, ringCapacity(&r), 4);
    KT_EQ(t, ringFreeSpace(&r), 4);

    KT_TRUE(t, ringPush(&r, 'a'));
    KT_TRUE(t, ringPush(&r, 'b'));
    KT_EQ(t, ringCount(&r), 2);
    KT_EQ(t, ringFreeSpace(&r), 2);

    uint8 out;
    KT_TRUE(t, ringPop(&r, &out));
    KT_EQ(t, out, 'a');
    KT_TRUE(t, ringPop(&r, &out));
    KT_EQ(t, out, 'b');
    KT_TRUE(t, ringIsEmpty(&r));
}

KTEST(ringFillsAndRejects)
{
    uint8 backing[3];
    RingBuffer r;
    ringInit(&r, backing, sizeof(backing));

    KT_TRUE(t, ringPush(&r, 1));
    KT_TRUE(t, ringPush(&r, 2));
    KT_TRUE(t, ringPush(&r, 3));
    KT_TRUE(t, ringIsFull(&r));
    KT_FALSE(t, ringPush(&r, 4));
    KT_EQ(t, ringCount(&r), 3);

    uint8 out;
    ringPop(&r, &out);
    KT_EQ(t, out, 1);
    KT_FALSE(t, ringIsFull(&r));
    KT_TRUE(t, ringPush(&r, 4));
}

KTEST(ringWrapsAroundCorrectly)
{
    uint8 backing[4];
    RingBuffer r;
    ringInit(&r, backing, sizeof(backing));

    ringPush(&r, 1);
    ringPush(&r, 2);
    ringPush(&r, 3);
    ringPush(&r, 4);
    uint8 out;
    ringPop(&r, &out);
    KT_EQ(t, out, 1);
    ringPop(&r, &out);
    KT_EQ(t, out, 2);
    ringPush(&r, 5);
    ringPush(&r, 6);

    ringPop(&r, &out);
    KT_EQ(t, out, 3);
    ringPop(&r, &out);
    KT_EQ(t, out, 4);
    ringPop(&r, &out);
    KT_EQ(t, out, 5);
    ringPop(&r, &out);
    KT_EQ(t, out, 6);
    KT_TRUE(t, ringIsEmpty(&r));
}

KTEST(ringBulkWriteRead)
{
    uint8 backing[8];
    RingBuffer r;
    ringInit(&r, backing, sizeof(backing));

    const uint8 msg[] = { 10, 20, 30, 40, 50 };
    usize written = ringWrite(&r, msg, sizeof(msg));
    KT_EQ(t, written, 5);
    KT_EQ(t, ringCount(&r), 5);

    const uint8 more[] = { 1, 2, 3, 4 };
    usize written2 = ringWrite(&r, more, sizeof(more));
    KT_EQ(t, written2, 3);
    KT_TRUE(t, ringIsFull(&r));

    uint8 outBuf[16];
    usize readCount = ringRead(&r, outBuf, sizeof(outBuf));
    KT_EQ(t, readCount, 8);
    KT_EQ(t, outBuf[0], 10);
    KT_EQ(t, outBuf[4], 50);
    KT_EQ(t, outBuf[5], 1);
    KT_EQ(t, outBuf[7], 3);
    KT_TRUE(t, ringIsEmpty(&r));
}

KTEST(ringClearResets)
{
    uint8 backing[4];
    RingBuffer r;
    ringInit(&r, backing, sizeof(backing));
    ringPush(&r, 1);
    ringPush(&r, 2);

    ringClear(&r);
    KT_TRUE(t, ringIsEmpty(&r));
    KT_EQ(t, ringCount(&r), 0);
    KT_EQ(t, ringFreeSpace(&r), 4);
}
