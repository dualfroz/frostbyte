#ifndef FROSTBYTE_RTC_H
#define FROSTBYTE_RTC_H

#include "types.h"

/*
 * CMOS real-time clock driver (MC146818-compatible, ports 0x70/0x71) --
 * the first source of real wall-clock date/time this kernel has ever had.
 * Every other notion of "time" in frostbyte (pit.h's tick counter,
 * threadSleep, userAlarm) is relative to boot, not to the calendar; this
 * is the piece that lets a process ask "what day is it" at all.
 *
 * Honest limitation, same spirit as syscall.c's own documented fork()-less
 * design: the CMOS century register's index is not standardized (it is
 * only discoverable via the ACPI FADT table, which this kernel does not
 * parse), so rtcReadDateTime hardcodes the century to 20 -- correct for
 * any date from 2000 through 2099, which covers every real boot this
 * kernel will ever see, but not a documented general-purpose solution.
 */

typedef struct RtcDateTime {
    int64 year;   /* full year, e.g. 2026 */
    int64 month;  /* 1-12 */
    int64 day;    /* 1-31 */
    int64 hour;   /* 0-23, already normalized out of 12-hour/PM if needed */
    int64 minute; /* 0-59 */
    int64 second; /* 0-59 */
} RtcDateTime;

/* Converts a single CMOS BCD byte (e.g. 0x59) to its binary value (59).
   Exposed on its own, like ata.c's ataModelString, because it is a real
   piece of the conversion pipeline worth testing in isolation from any
   hardware access. */
uint8 rtcBcdToBinary(uint8 bcd);

/* Reads the current date/time off the CMOS RTC. Polls Status Register A's
   Update-In-Progress bit (bounded, like ata.c's ataPollTimeout) before
   each of two back-to-back raw reads and only accepts the result once both
   raw reads agree byte-for-byte, which rules out a read torn mid-tick by
   the RTC's own once-a-second update. Returns false only if the two raw
   reads never agreed within the bounded retry budget (never hangs). */
bool rtcReadDateTime(RtcDateTime* out);

/* Civil calendar date/time -> seconds since the Unix epoch
   (1970-01-01T00:00:00Z), via Howard Hinnant's days_from_civil algorithm
   (proleptic Gregorian, correct for any year the RtcDateTime fields above
   can represent). Pure arithmetic, no hardware access -- deterministic and
   testable without a real clock. */
int64 rtcEpochSeconds(const RtcDateTime* dt);

/* rtcReadDateTime + rtcEpochSeconds in one call, the usual entry point for
   anything that just wants "now" as a plain integer (sysTime, syscall.c).
   Returns 0 if the underlying read failed. */
int64 rtcNow(void);

/* Seconds since the Unix epoch -> civil calendar date/time, the exact
   inverse of rtcEpochSeconds above (Howard Hinnant's civil_from_days,
   proleptic Gregorian, same algorithm family as daysFromCivil in rtc.c).
   Correct for any epochSeconds value representable in int64, including
   negative ones (dates before 1970). Pure arithmetic, no hardware access
   -- the piece sfs.h's on-disk mtime/ctime (raw epoch integers) needs to
   become a human-readable date, e.g. for a shell `ls -l`. */
void rtcCivilFromEpoch(int64 epochSeconds, RtcDateTime* out);

#endif
