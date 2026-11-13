#include "nb/time.h"

#include "platform/nb_platform.h"

#include <string.h>
#include <time.h>

int64_t nb_time_now_ns(void)
{
    return nb_platform_time_realtime_ns();
}

int64_t nb_time_monotonic_ns(void)
{
    return nb_platform_time_monotonic_ns();
}

int64_t nb_time_now_ms(void)
{
    return nb_time_now_ns() / INT64_C(1000000);
}

void nb_time_sleep_ms(uint32_t ms)
{
    nb_platform_sleep_ms(ms);
}

static bool is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month)
{
    static const int days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days[month - 1];
}

/* Howard Hinnant's days_from_civil, with 1970-01-01 as day zero. */
static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2u;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153u * (month + (month > 2u ? (unsigned)-3 : 9u)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

bool nb_time_format_rfc3339(int64_t ns, char *buf, size_t cap)
{
    if (!buf || cap == 0) {
        return false;
    }
    int64_t seconds = ns / INT64_C(1000000000);
    int64_t nanos = ns % INT64_C(1000000000);
    if (nanos < 0) {
        nanos += INT64_C(1000000000);
        seconds--;
    }

    time_t sec = (time_t)seconds;
    struct tm tmv;
    if (!nb_platform_localtime((int64_t)sec, &tmv)) {
        return false;
    }

    int32_t offset = 0;
    if (!nb_platform_local_offset_seconds((int64_t)sec, &offset)) {
        return false;
    }
    char sign = offset < 0 ? '-' : '+';
    int64_t absolute = offset < 0 ? -(int64_t)offset : (int64_t)offset;
    int offset_hours = (int)(absolute / 3600);
    int offset_minutes = (int)((absolute % 3600) / 60);

    int written = snprintf(buf, cap,
                           "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d:%02d",
                           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                           tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                           (int)(nanos / INT64_C(1000000)),
                           sign, offset_hours, offset_minutes);
    return written > 0 && (size_t)written < cap;
}

bool nb_time_format_rfc3339_utc(int64_t ns, char *buf, size_t cap)
{
    if (!buf || cap == 0) {
        return false;
    }
    int64_t seconds = ns / INT64_C(1000000000);
    int64_t nanos = ns % INT64_C(1000000000);
    if (nanos < 0) {
        nanos += INT64_C(1000000000);
        seconds--;
    }

    time_t sec = (time_t)seconds;
    struct tm tmv;
    if (!nb_platform_gmtime((int64_t)sec, &tmv)) {
        return false;
    }

    int written = snprintf(buf, cap,
                           "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                           tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                           (int)(nanos / INT64_C(1000000)));
    return written > 0 && (size_t)written < cap;
}

static bool is_digit_char(char c)
{
    return c >= '0' && c <= '9';
}

static bool parse_rfc3339_impl(const char *text, size_t len, int64_t *ns_out)
{
    if (!text || !ns_out || len < 20u) {
        return false;
    }

    for (size_t i = 0; i < 19u; i++) {
        if (i == 4u || i == 7u) {
            if (text[i] != '-') {
                return false;
            }
        } else if (i == 10u) {
            if (text[i] != 'T') {
                return false;
            }
        } else if (i == 13u || i == 16u) {
            if (text[i] != ':') {
                return false;
            }
        } else if (!is_digit_char(text[i])) {
            return false;
        }
    }

    int year = (text[0] - '0') * 1000 + (text[1] - '0') * 100 +
               (text[2] - '0') * 10 + (text[3] - '0');
    int month = (text[5] - '0') * 10 + (text[6] - '0');
    int day = (text[8] - '0') * 10 + (text[9] - '0');
    int hour = (text[11] - '0') * 10 + (text[12] - '0');
    int minute = (text[14] - '0') * 10 + (text[15] - '0');
    int second = (text[17] - '0') * 10 + (text[18] - '0');
    if (year < 1 || year > 9999 ||
        month < 1 || month > 12 ||
        day < 1 || day > days_in_month(year, month) ||
        hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return false;
    }

    size_t p = 19u;
    int64_t fraction = 0;
    if (p < len && text[p] == '.') {
        p++;
        int digits = 0;
        while (p < len && is_digit_char(text[p])) {
            if (digits < 9) {
                fraction = fraction * 10 + (text[p] - '0');
                digits++;
            }
            p++;
        }
        if (digits == 0) {
            return false;
        }
        while (digits < 9) {
            fraction *= 10;
            digits++;
        }
    }

    int offset_seconds = 0;
    if (p < len && (text[p] == 'Z' || text[p] == 'z')) {
        p++;
    } else if (p < len && (text[p] == '+' || text[p] == '-')) {
        int sign = text[p] == '+' ? 1 : -1;
        if (p + 6u > len ||
            !is_digit_char(text[p + 1u]) || !is_digit_char(text[p + 2u]) ||
            text[p + 3u] != ':' ||
            !is_digit_char(text[p + 4u]) || !is_digit_char(text[p + 5u])) {
            return false;
        }
        int offset_hours =
            (text[p + 1u] - '0') * 10 + (text[p + 2u] - '0');
        int offset_minutes =
            (text[p + 4u] - '0') * 10 + (text[p + 5u] - '0');
        if (offset_hours > 23 || offset_minutes > 59) {
            return false;
        }
        offset_seconds = sign * (offset_hours * 3600 + offset_minutes * 60);
        p += 6;
    } else {
        return false;
    }
    if (p != len) {
        return false;
    }

    int64_t days = days_from_civil(year, (unsigned)month, (unsigned)day);
    int64_t seconds = days * INT64_C(86400) +
                      (int64_t)hour * 3600 + (int64_t)minute * 60 +
                      (int64_t)second - offset_seconds;
    const int64_t billion = INT64_C(1000000000);
    const int64_t min_seconds = INT64_MIN / billion - 1;
    const int64_t min_fraction = billion + INT64_MIN % billion;
    if (seconds > (INT64_MAX - fraction) / billion ||
        seconds < min_seconds ||
        (seconds == min_seconds && fraction < min_fraction)) {
        return false;
    }
    /* The floor second at INT64_MIN cannot be multiplied by a billion on
     * its own; shift one second into the fraction before multiplication. */
    *ns_out = seconds < 0 ? (seconds + 1) * billion + (fraction - billion)
                         : seconds * billion + fraction;
    return true;
}

bool nb_time_parse_rfc3339(const char *text, int64_t *ns_out)
{
    if (!text || !ns_out) {
        return false;
    }
    return parse_rfc3339_impl(text, strlen(text), ns_out);
}

bool nb_time_parse_rfc3339_str(nb_str text, int64_t *ns_out)
{
    if (text.len != 0 && !text.ptr) {
        return false;
    }
    return parse_rfc3339_impl(text.ptr, text.len, ns_out);
}

int64_t nb_time_monotonic_ms(void) { return nb_time_monotonic_ns() / INT64_C(1000000); }

int64_t nb_time_deadline_after_ns(int64_t timeout_ns)
{
    int64_t now = nb_time_monotonic_ns();
    if (timeout_ns <= 0) {
        return now;
    }
    return timeout_ns > INT64_MAX - now ? INT64_MAX : now + timeout_ns;
}

int64_t nb_time_remaining_ns(int64_t deadline_ns)
{
    int64_t now = nb_time_monotonic_ns();
    return deadline_ns <= now ? 0 : deadline_ns - now;
}
