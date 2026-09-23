#include "rtc.h"
#include "ports.h"
#include "string.h"

#define rtcPortIndex 0x70
#define rtcPortData  0x71

#define rtcRegSeconds  0x00
#define rtcRegMinutes  0x02
#define rtcRegHours    0x04
#define rtcRegDay      0x07
#define rtcRegMonth    0x08
#define rtcRegYear     0x09
#define rtcRegStatusA  0x0A
#define rtcRegStatusB  0x0B

#define rtcStatusAUip  0x80

#define rtcStatusBHourFormat24 0x02
#define rtcStatusBBinaryMode   0x04
#define rtcHourPmBit           0x80

#define rtcUipTimeout 100000u
#define rtcReadAttempts 8u

#define rtcAssumedCentury 20

static uint8 cmosRead(uint8 reg)
{
    outb(rtcPortIndex, (uint8)(reg | 0x80));
    ioWait();
    return inb(rtcPortData);
}

static bool waitWhileUpdateInProgress(void)
{
    for (uint32 i = 0; i < rtcUipTimeout; i++) {
        if ((cmosRead(rtcRegStatusA) & rtcStatusAUip) == 0)
            return true;
    }
    return false;
}

typedef struct RtcRaw {
    uint8 seconds;
    uint8 minutes;
    uint8 hours;
    uint8 day;
    uint8 month;
    uint8 year;
} RtcRaw;

static void readRawOnce(RtcRaw* out)
{
    out->seconds = cmosRead(rtcRegSeconds);
    out->minutes = cmosRead(rtcRegMinutes);
    out->hours = cmosRead(rtcRegHours);
    out->day = cmosRead(rtcRegDay);
    out->month = cmosRead(rtcRegMonth);
    out->year = cmosRead(rtcRegYear);
}

uint8 rtcBcdToBinary(uint8 bcd)
{
    return (uint8)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

bool rtcReadDateTime(RtcDateTime* out)
{
    RtcRaw a;
    RtcRaw b;
    bool agree = false;

    for (uint32 attempt = 0; attempt < rtcReadAttempts && !agree; attempt++) {
        if (!waitWhileUpdateInProgress())
            return false;
        readRawOnce(&a);
        if (!waitWhileUpdateInProgress())
            return false;
        readRawOnce(&b);
        agree = memcmp(&a, &b, sizeof(RtcRaw)) == 0;
    }
    if (!agree)
        return false;

    uint8 statusB = cmosRead(rtcRegStatusB);
    bool binaryMode = (statusB & rtcStatusBBinaryMode) != 0;
    bool is24Hour = (statusB & rtcStatusBHourFormat24) != 0;

    bool pm = (a.hours & rtcHourPmBit) != 0;
    uint8 rawHours = (uint8)(a.hours & (uint8)~rtcHourPmBit);

    uint8 seconds = binaryMode ? a.seconds : rtcBcdToBinary(a.seconds);
    uint8 minutes = binaryMode ? a.minutes : rtcBcdToBinary(a.minutes);
    uint8 hours = binaryMode ? rawHours : rtcBcdToBinary(rawHours);
    uint8 day = binaryMode ? a.day : rtcBcdToBinary(a.day);
    uint8 month = binaryMode ? a.month : rtcBcdToBinary(a.month);
    uint8 year = binaryMode ? a.year : rtcBcdToBinary(a.year);

    if (!is24Hour) {
        if (pm && hours != 12)
            hours = (uint8)(hours + 12);
        else if (!pm && hours == 12)
            hours = 0;
    }

    out->year = rtcAssumedCentury * 100 + year;
    out->month = month;
    out->day = day;
    out->hour = hours;
    out->minute = minutes;
    out->second = seconds;
    return true;
}

static int64 daysFromCivil(int64 y, int64 m, int64 d)
{
    y -= (m <= 2) ? 1 : 0;
    int64 era = (y >= 0 ? y : y - 399) / 400;
    int64 yoe = y - era * 400;
    int64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64 rtcEpochSeconds(const RtcDateTime* dt)
{
    int64 days = daysFromCivil(dt->year, dt->month, dt->day);
    return days * 86400 + dt->hour * 3600 + dt->minute * 60 + dt->second;
}

int64 rtcNow(void)
{
    RtcDateTime dt;
    if (!rtcReadDateTime(&dt))
        return 0;
    return rtcEpochSeconds(&dt);
}

static void civilFromDays(int64 z, int64* year, int64* month, int64* day)
{
    z += 719468;
    int64 era = (z >= 0 ? z : z - 146096) / 146097;
    int64 doe = z - era * 146097;
    int64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64 y = yoe + era * 400;
    int64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64 mp = (5 * doy + 2) / 153;
    int64 d = doy - (153 * mp + 2) / 5 + 1;
    int64 m = mp + (mp < 10 ? 3 : -9);
    *year = y + (m <= 2 ? 1 : 0);
    *month = m;
    *day = d;
}

void rtcCivilFromEpoch(int64 epochSeconds, RtcDateTime* out)
{
    int64 days = epochSeconds / 86400;
    int64 secondsOfDay = epochSeconds % 86400;
    if (secondsOfDay < 0) {
        secondsOfDay += 86400;
        days -= 1;
    }

    civilFromDays(days, &out->year, &out->month, &out->day);
    out->hour = secondsOfDay / 3600;
    out->minute = (secondsOfDay % 3600) / 60;
    out->second = secondsOfDay % 60;
}
