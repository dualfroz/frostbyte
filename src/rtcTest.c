#include "ktest.h"
#include "rtc.h"

KTEST(rtcBcdToBinaryKnownValues)
{
    KT_EQ(t, rtcBcdToBinary(0x00), 0);
    KT_EQ(t, rtcBcdToBinary(0x09), 9);
    KT_EQ(t, rtcBcdToBinary(0x10), 10);
    KT_EQ(t, rtcBcdToBinary(0x23), 23);
    KT_EQ(t, rtcBcdToBinary(0x59), 59);
}

KTEST(rtcEpochSecondsUnixEpochIsZero)
{
    RtcDateTime dt = { .year = 1970, .month = 1, .day = 1, .hour = 0, .minute = 0, .second = 0 };
    KT_EQ(t, rtcEpochSeconds(&dt), 0);
}

KTEST(rtcEpochSecondsTimeOfDayComponent)
{
    RtcDateTime dt = { .year = 1970, .month = 1, .day = 1, .hour = 1, .minute = 2, .second = 3 };
    KT_EQ(t, rtcEpochSeconds(&dt), 1 * 3600 + 2 * 60 + 3);
}

KTEST(rtcEpochSecondsOrdinaryDayIsOneDayApart)
{
    RtcDateTime a = { .year = 2026, .month = 9, .day = 7, .hour = 0, .minute = 0, .second = 0 };
    RtcDateTime b = { .year = 2026, .month = 9, .day = 8, .hour = 0, .minute = 0, .second = 0 };
    KT_EQ(t, rtcEpochSeconds(&b) - rtcEpochSeconds(&a), 86400);
}

KTEST(rtcEpochSecondsLeapYearFebruaryHas29Days)
{
    RtcDateTime feb28 = { .year = 2024, .month = 2, .day = 28, .hour = 0, .minute = 0, .second = 0 };
    RtcDateTime mar1 = { .year = 2024, .month = 3, .day = 1, .hour = 0, .minute = 0, .second = 0 };
    KT_EQ(t, rtcEpochSeconds(&mar1) - rtcEpochSeconds(&feb28), 2 * 86400);
}

KTEST(rtcEpochSecondsNonLeapYearFebruaryHas28Days)
{
    RtcDateTime feb28 = { .year = 2023, .month = 2, .day = 28, .hour = 0, .minute = 0, .second = 0 };
    RtcDateTime mar1 = { .year = 2023, .month = 3, .day = 1, .hour = 0, .minute = 0, .second = 0 };
    KT_EQ(t, rtcEpochSeconds(&mar1) - rtcEpochSeconds(&feb28), 86400);
}

KTEST(rtcEpochSecondsCenturyLeapRule)
{
    RtcDateTime feb28_2000 = { .year = 2000, .month = 2, .day = 28, .hour = 0, .minute = 0, .second = 0 };
    RtcDateTime mar1_2000 = { .year = 2000, .month = 3, .day = 1, .hour = 0, .minute = 0, .second = 0 };
    KT_EQ(t, rtcEpochSeconds(&mar1_2000) - rtcEpochSeconds(&feb28_2000), 2 * 86400);

    RtcDateTime feb28_1900 = { .year = 1900, .month = 2, .day = 28, .hour = 0, .minute = 0, .second = 0 };
    RtcDateTime mar1_1900 = { .year = 1900, .month = 3, .day = 1, .hour = 0, .minute = 0, .second = 0 };
    KT_EQ(t, rtcEpochSeconds(&mar1_1900) - rtcEpochSeconds(&feb28_1900), 86400);
}

KTEST(rtcCivilFromEpochUnixEpochIsZero)
{
    RtcDateTime dt;
    rtcCivilFromEpoch(0, &dt);
    KT_EQ(t, dt.year, 1970);
    KT_EQ(t, dt.month, 1);
    KT_EQ(t, dt.day, 1);
    KT_EQ(t, dt.hour, 0);
    KT_EQ(t, dt.minute, 0);
    KT_EQ(t, dt.second, 0);
}

KTEST(rtcCivilFromEpochTimeOfDayComponent)
{
    RtcDateTime dt;
    rtcCivilFromEpoch(1 * 3600 + 2 * 60 + 3, &dt);
    KT_EQ(t, dt.year, 1970);
    KT_EQ(t, dt.month, 1);
    KT_EQ(t, dt.day, 1);
    KT_EQ(t, dt.hour, 1);
    KT_EQ(t, dt.minute, 2);
    KT_EQ(t, dt.second, 3);
}

KTEST(rtcCivilFromEpochNegativeBeforeEpoch)
{
    RtcDateTime dt;
    rtcCivilFromEpoch(-1, &dt);
    KT_EQ(t, dt.year, 1969);
    KT_EQ(t, dt.month, 12);
    KT_EQ(t, dt.day, 31);
    KT_EQ(t, dt.hour, 23);
    KT_EQ(t, dt.minute, 59);
    KT_EQ(t, dt.second, 59);

    rtcCivilFromEpoch(-86400, &dt);
    KT_EQ(t, dt.year, 1969);
    KT_EQ(t, dt.month, 12);
    KT_EQ(t, dt.day, 31);
    KT_EQ(t, dt.hour, 0);
    KT_EQ(t, dt.minute, 0);
    KT_EQ(t, dt.second, 0);
}

KTEST(rtcCivilFromEpochLeapDayRoundTrip)
{
    RtcDateTime leapDay = { .year = 2024, .month = 2, .day = 29, .hour = 12, .minute = 30, .second = 45 };
    int64 epoch = rtcEpochSeconds(&leapDay);

    RtcDateTime back;
    rtcCivilFromEpoch(epoch, &back);
    KT_EQ(t, back.year, 2024);
    KT_EQ(t, back.month, 2);
    KT_EQ(t, back.day, 29);
    KT_EQ(t, back.hour, 12);
    KT_EQ(t, back.minute, 30);
    KT_EQ(t, back.second, 45);
}

KTEST(rtcCivilFromEpochRoundTripsAcrossManyDates)
{
    RtcDateTime samples[] = {
        { 1970, 1, 1, 0, 0, 0 },
        { 1999, 12, 31, 23, 59, 59 },
        { 2000, 1, 1, 0, 0, 1 },
        { 2000, 2, 29, 6, 7, 8 },
        { 2023, 2, 28, 18, 0, 0 },
        { 2023, 3, 1, 0, 0, 0 },
        { 2026, 9, 7, 17, 30, 0 },
        { 2099, 12, 31, 23, 59, 59 },
    };
    for (uint64 i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        int64 epoch = rtcEpochSeconds(&samples[i]);
        RtcDateTime back;
        rtcCivilFromEpoch(epoch, &back);
        KT_EQ(t, back.year, samples[i].year);
        KT_EQ(t, back.month, samples[i].month);
        KT_EQ(t, back.day, samples[i].day);
        KT_EQ(t, back.hour, samples[i].hour);
        KT_EQ(t, back.minute, samples[i].minute);
        KT_EQ(t, back.second, samples[i].second);
    }
}

KTEST(rtcReadDateTimeReturnsPlausibleFields)
{
    RtcDateTime dt;
    KT_TRUE(t, rtcReadDateTime(&dt));
    KT_GE(t, dt.year, 2020);
    KT_LE(t, dt.year, 2099);
    KT_GE(t, dt.month, 1);
    KT_LE(t, dt.month, 12);
    KT_GE(t, dt.day, 1);
    KT_LE(t, dt.day, 31);
    KT_GE(t, dt.hour, 0);
    KT_LT(t, dt.hour, 24);
    KT_GE(t, dt.minute, 0);
    KT_LT(t, dt.minute, 60);
    KT_GE(t, dt.second, 0);
    KT_LT(t, dt.second, 60);
}

KTEST(rtcNowIsNeverNegativeOrObviouslyWrong)
{
    int64 now = rtcNow();
    KT_GT(t, now, 0);
    KT_GT(t, now, 1577836800);
}

KTEST(rtcNowIsMonotonicAcrossConsecutiveReads)
{
    int64 first = rtcNow();
    for (volatile int i = 0; i < 20000; i++) { }
    int64 second = rtcNow();
    KT_GE(t, second, first);
}
