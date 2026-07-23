#include "ktest.h"
#include "klib.h"
#include "string.h"

KTEST(klibCtype)
{
    KT_TRUE(t, isDigit('0'));
    KT_TRUE(t, isDigit('9'));
    KT_FALSE(t, isDigit('a'));
    KT_TRUE(t, isAlpha('a'));
    KT_TRUE(t, isAlpha('Z'));
    KT_FALSE(t, isAlpha('5'));
    KT_TRUE(t, isAlnum('7'));
    KT_TRUE(t, isAlnum('q'));
    KT_TRUE(t, isSpace(' '));
    KT_TRUE(t, isSpace('\t'));
    KT_FALSE(t, isSpace('x'));
    KT_TRUE(t, isHexDigit('f'));
    KT_TRUE(t, isHexDigit('F'));
    KT_TRUE(t, isHexDigit('9'));
    KT_FALSE(t, isHexDigit('g'));
    KT_EQ(t, toUpper('a'), 'A');
    KT_EQ(t, toUpper('A'), 'A');
    KT_EQ(t, toLower('Z'), 'z');
    KT_EQ(t, toLower('5'), '5');
}

KTEST(klibStrCopyCat)
{
    char buf[32];
    strcpy(buf, "frost");
    KT_STREQ(t, buf, "frost");
    strcat(buf, "byte");
    KT_STREQ(t, buf, "frostbyte");

    char nbuf[8];
    memset(nbuf, 'X', sizeof(nbuf));
    strncpy(nbuf, "hi", sizeof(nbuf));
    KT_EQ(t, nbuf[0], 'h');
    KT_EQ(t, nbuf[1], 'i');
    KT_EQ(t, nbuf[2], 0);
    KT_EQ(t, nbuf[7], 0);   /* strncpy pads the remainder with nul */

    char abuf[16];
    strcpy(abuf, "ab");
    strncat(abuf, "cdefgh", 3);
    KT_STREQ(t, abuf, "abcde");
}

KTEST(klibStrSearch)
{
    const char* s = "the quick brown fox";
    KT_EQP(t, strchr(s, 'q'), s + 4);
    KT_NULL(t, strchr(s, 'Z'));
    KT_EQP(t, strrchr(s, 'o'), s + 17);
    KT_NOTNULL(t, strstr(s, "brown"));
    KT_STREQ(t, strstr(s, "brown"), "brown fox");
    KT_NULL(t, strstr(s, "purple"));
    KT_STREQ(t, strstr(s, ""), s);   /* empty needle matches at start */

    KT_EQ(t, strspn("aaabbbccc", "ab"), 6);
    KT_EQ(t, strcspn("hello, world", ", "), 5);
}

KTEST(klibStrTokR)
{
    char input[32];
    strcpy(input, "one,two,,four");
    char* save = nullptr;

    char* tok = strTokR(input, ",", &save);
    KT_STREQ(t, tok, "one");
    tok = strTokR(nullptr, ",", &save);
    KT_STREQ(t, tok, "two");
    tok = strTokR(nullptr, ",", &save);
    KT_STREQ(t, tok, "four");   /* empty field between two commas is skipped */
    tok = strTokR(nullptr, ",", &save);
    KT_NULL(t, tok);
}

KTEST(klibNumberParsing)
{
    KT_EQ(t, atoi("42"), 42);
    KT_EQ(t, atoi("-42"), -42);
    KT_EQ(t, atoi("  7"), 7);
    KT_EQ(t, atol("123456789012"), 123456789012LL);

    char* end = nullptr;
    KT_EQ(t, strtol("2026", &end, 10), 2026);
    KT_EQ(t, *end, 0);

    KT_EQ(t, strtol("-15", nullptr, 10), -15);
    KT_EQ(t, strtol("ff", nullptr, 16), 255);
    KT_EQ(t, strtol("0x1A", nullptr, 0), 26);
    KT_EQ(t, strtol("010", nullptr, 0), 8);   /* auto-detected octal */
    KT_EQ(t, strtol("42", nullptr, 0), 42);

    KT_EQU(t, strtoul("ffff", nullptr, 16), 65535);

    end = nullptr;
    strtol("nope", &end, 10);
    KT_STREQ(t, end, "nope");   /* no digits consumed: endptr == start */
}

static int intCmp(const void* a, const void* b)
{
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    return (ia > ib) - (ia < ib);
}

KTEST(klibQsortBsearch)
{
    int values[8] = { 5, 3, 8, 1, 9, 2, 7, 4 };
    qsort(values, 8, sizeof(int), intCmp);
    for (int i = 0; i < 7; i++)
        KT_LE(t, values[i], values[i + 1]);
    KT_EQ(t, values[0], 1);
    KT_EQ(t, values[7], 9);

    int key = 7;
    int* found = (int*)bsearch(&key, values, 8, sizeof(int), intCmp);
    KT_NOTNULL(t, found);
    KT_EQ(t, *found, 7);

    int missing = 42;
    KT_NULL(t, bsearch(&missing, values, 8, sizeof(int), intCmp));
}

KTEST(klibSnprintf)
{
    char buf[64];

    int n = snprintf(buf, sizeof(buf), "%s=%d", "answer", 42);
    KT_STREQ(t, buf, "answer=42");
    KT_EQ(t, n, 9);

    snprintf(buf, sizeof(buf), "%05d", 42);
    KT_STREQ(t, buf, "00042");

    snprintf(buf, sizeof(buf), "%05d", -42);
    KT_STREQ(t, buf, "-0042");

    snprintf(buf, sizeof(buf), "%x-%X", 255, 255);
    KT_STREQ(t, buf, "ff-FF");

    snprintf(buf, sizeof(buf), "%lu", 9876543210ULL);
    KT_STREQ(t, buf, "9876543210");

    snprintf(buf, sizeof(buf), "%c%c%c", 'a', 'b', 'c');
    KT_STREQ(t, buf, "abc");

    snprintf(buf, sizeof(buf), "100%%");
    KT_STREQ(t, buf, "100%");

    /* truncation: buffer smaller than the formatted result still nul
       terminates, and reports the length that would have been written */
    char small[4];
    int wanted = snprintf(small, sizeof(small), "hello");
    KT_STREQ(t, small, "hel");
    KT_EQ(t, wanted, 5);

    /* size == 0 must not touch the buffer at all, and still reports length */
    int wantedZero = snprintf(nullptr, 0, "hello");
    KT_EQ(t, wantedZero, 5);
}
