#include "ktest.h"
#include "ata.h"
#include "string.h"

static void fillPattern(uint8* buf, uint64 seed)
{
    for (int i = 0; i < ataSectorSize; i++)
        buf[i] = (uint8)((seed * 7 + (uint64)i * 13) & 0xFF);
}

KTEST(ataDrivePresent)
{
    KT_TRUE(t, ataPresent());
    KT_GT(t, ataSectorCount(), 0);

    const char* model = ataModelString();
    KT_NOTNULL(t, model);
    KT_TRUE(t, strlen(model) > 0);
}

KTEST(ataSingleSectorRoundTrip)
{
    uint8 out[ataSectorSize];
    uint8 in[ataSectorSize];
    fillPattern(out, 0xA5);

    KT_TRUE(t, ataWriteSectors(10, 1, out));
    memset(in, 0, sizeof(in));
    KT_TRUE(t, ataReadSectors(10, 1, in));
    KT_EQ(t, memcmp(in, out, ataSectorSize), 0);
}

KTEST(ataOverwriteReflectsLatestData)
{
    uint8 first[ataSectorSize];
    uint8 second[ataSectorSize];
    uint8 back[ataSectorSize];
    fillPattern(first, 0x11);
    fillPattern(second, 0x99);

    KT_TRUE(t, ataWriteSectors(11, 1, first));
    KT_TRUE(t, ataWriteSectors(11, 1, second));
    KT_TRUE(t, ataReadSectors(11, 1, back));
    KT_EQ(t, memcmp(back, second, ataSectorSize), 0);
    KT_NE(t, memcmp(back, first, ataSectorSize), 0);
}

KTEST(ataAdjacentSectorsDoNotCrossTalk)
{
    uint8 a[ataSectorSize];
    uint8 b[ataSectorSize];
    uint8 backA[ataSectorSize];
    uint8 backB[ataSectorSize];
    fillPattern(a, 0x30);
    fillPattern(b, 0x77);

    KT_TRUE(t, ataWriteSectors(20, 1, a));
    KT_TRUE(t, ataWriteSectors(21, 1, b));
    KT_TRUE(t, ataReadSectors(20, 1, backA));
    KT_TRUE(t, ataReadSectors(21, 1, backB));
    KT_EQ(t, memcmp(backA, a, ataSectorSize), 0);
    KT_EQ(t, memcmp(backB, b, ataSectorSize), 0);
    KT_NE(t, memcmp(backA, backB, ataSectorSize), 0);
}

KTEST(ataMultiSectorRoundTrip)
{
    #define ataTestSectors 4
    uint8 out[ataTestSectors * ataSectorSize];
    uint8 in[ataTestSectors * ataSectorSize];

    for (int s = 0; s < ataTestSectors; s++)
        fillPattern(out + s * ataSectorSize, (uint64)(0x40 + s));

    KT_TRUE(t, ataWriteSectors(30, ataTestSectors, out));
    memset(in, 0, sizeof(in));
    KT_TRUE(t, ataReadSectors(30, ataTestSectors, in));

    for (int s = 0; s < ataTestSectors; s++)
        KT_EQ(t, memcmp(in + s * ataSectorSize, out + s * ataSectorSize, ataSectorSize), 0);
    #undef ataTestSectors
}

KTEST(ataLastSectorIsAddressable)
{
    uint64 last = ataSectorCount() - 1;
    uint8 out[ataSectorSize];
    uint8 in[ataSectorSize];
    fillPattern(out, 0xEE);

    KT_TRUE(t, ataWriteSectors(last, 1, out));
    KT_TRUE(t, ataReadSectors(last, 1, in));
    KT_EQ(t, memcmp(in, out, ataSectorSize), 0);
}

KTEST(ataOutOfRangeRequestsAreRejected)
{
    uint8 buf[ataSectorSize];
    KT_FALSE(t, ataReadSectors(ataSectorCount(), 1, buf));
    KT_FALSE(t, ataWriteSectors(ataSectorCount(), 1, buf));

    uint8 twoSectors[2 * ataSectorSize];
    KT_FALSE(t, ataReadSectors(ataSectorCount() - 1, 2, twoSectors));
}

KTEST(ataZeroLengthRequestIsNoop)
{
    uint8 buf[ataSectorSize];
    KT_TRUE(t, ataReadSectors(0, 0, buf));
    KT_TRUE(t, ataWriteSectors(0, 0, buf));
}
