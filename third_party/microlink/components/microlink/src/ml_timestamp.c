#include "ml_timestamp.h"
#include <string.h>

static int digits(const char *text, unsigned count)
{
    int value = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (text[i] < '0' || text[i] > '9') return -1;
        value = value * 10 + text[i] - '0';
    }
    return value;
}

bool ml_timestamp_parse(const char *text, int64_t *epoch)
{
    if (!text || !epoch || strlen(text) < 20 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') return false;
    int year = digits(text, 4), month = digits(text + 5, 2), day = digits(text + 8, 2);
    int hour = digits(text + 11, 2), minute = digits(text + 14, 2), second = digits(text + 17, 2);
    if (year < 1 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) return false;
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    static const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (day > days[month - 1] + (month == 2 && leap)) return false;
    const char *zone = text + 19;
    if (*zone == '.') {
        unsigned count = 0;
        ++zone;
        while (*zone >= '0' && *zone <= '9' && count < 9) { ++zone; ++count; }
        if (!count) return false;
    }
    int offset = 0;
    if (*zone == 'Z' && !zone[1]) {
        if (!strcmp(text, "0001-01-01T00:00:00Z")) { *epoch = 0; return true; }
    } else {
        if ((*zone != '+' && *zone != '-') || strlen(zone) != 6 || zone[3] != ':') return false;
        int hours = digits(zone + 1, 2), minutes = digits(zone + 4, 2);
        if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) return false;
        offset = (hours * 60 + minutes) * 60 * (*zone == '+' ? 1 : -1);
    }
    int64_t prior = year - 1;
    int64_t total = 365 * (int64_t)(year - 1970) + prior / 4 - prior / 100 + prior / 400 -
                    (1969 / 4 - 1969 / 100 + 1969 / 400);
    for (int i = 1; i < month; ++i) total += days[i - 1] + (i == 2 && leap);
    *epoch = (total + day - 1) * 86400 + hour * 3600 + minute * 60 + second - offset;
    return true;
}
