#ifndef FROSTBYTE_RTC_H
#define FROSTBYTE_RTC_H

#include "types.h"

typedef struct RtcDateTime {
    int64 year;
    int64 month;
    int64 day;
    int64 hour;
    int64 minute;
    int64 second;
} RtcDateTime;

uint8 rtcBcdToBinary(uint8 bcd);

bool rtcReadDateTime(RtcDateTime* out);

int64 rtcEpochSeconds(const RtcDateTime* dt);

int64 rtcNow(void);

void rtcCivilFromEpoch(int64 epochSeconds, RtcDateTime* out);

#endif
