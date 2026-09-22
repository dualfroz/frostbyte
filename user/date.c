#include "ulib.h"

static void printErr(const char* text)
{
    userWrite(2, text, stringLength(text));
}

static void printPadded(long value, int width)
{
    char digits[16];
    int len = 0;
    long remaining = value;

    if (remaining == 0)
        digits[len++] = '0';
    while (remaining > 0) {
        digits[len++] = (char)('0' + (remaining % 10));
        remaining /= 10;
    }
    while (len < width)
        digits[len++] = '0';

    for (int i = len - 1; i >= 0; i--)
        userWrite(1, &digits[i], 1);
}

static long daysFromCivil(long y, long m, long d)
{
    y -= (m <= 2) ? 1 : 0;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civilFromDays(long z, long* outYear, long* outMonth, long* outDay)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long dd = doy - (153 * mp + 2) / 5 + 1;
    long mm = mp + (mp < 10 ? 3 : -9);

    *outYear = yy + (mm <= 2 ? 1 : 0);
    *outMonth = mm;
    *outDay = dd;
}

int main(void)
{
    long epoch = userTime();
    if (epoch <= 0) {
        printErr("date: FAIL sysTime returned a non-positive epoch\n");
        return 1;
    }

    long days = epoch / 86400;
    long secondsOfDay = epoch % 86400;
    long hour = secondsOfDay / 3600;
    long minute = (secondsOfDay % 3600) / 60;
    long second = secondsOfDay % 60;

    long year;
    long month;
    long day;
    civilFromDays(days, &year, &month, &day);

    if (year < 2020 || year > 2099) {
        printErr("date: FAIL implausible year decoded\n");
        return 1;
    }

    printPadded(year, 4);
    userWrite(1, "-", 1);
    printPadded(month, 2);
    userWrite(1, "-", 1);
    printPadded(day, 2);
    userWrite(1, " ", 1);
    printPadded(hour, 2);
    userWrite(1, ":", 1);
    printPadded(minute, 2);
    userWrite(1, ":", 1);
    printPadded(second, 2);
    userWrite(1, "\n", 1);
    printErr("date: printed-ok\n");

    long roundTrip = daysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
    if (roundTrip != epoch) {
        printErr("date: FAIL roundtrip mismatch\n");
        return 1;
    }
    printErr("date: roundtrip-ok\n");

    return 0;
}
