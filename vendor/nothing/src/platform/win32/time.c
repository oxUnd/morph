#include "platform/nb_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string.h>
#include <time.h>

#define NB_UNIX_EPOCH_FILETIME INT64_C(116444736000000000)

int64_t nb_platform_time_realtime_ns(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return ((int64_t)value.QuadPart - NB_UNIX_EPOCH_FILETIME) * 100;
}

int64_t nb_platform_time_monotonic_ns(void)
{
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0) {
        return (int64_t)GetTickCount64() * INT64_C(1000000);
    }
    int64_t whole = counter.QuadPart / frequency.QuadPart;
    int64_t remainder = counter.QuadPart % frequency.QuadPart;
    return whole * INT64_C(1000000000) +
           remainder * INT64_C(1000000000) / frequency.QuadPart;
}

void nb_platform_sleep_ms(uint32_t ms)
{
    Sleep((DWORD)ms);
}

bool nb_platform_localtime(int64_t seconds, struct tm *out)
{
    if (!out) {
        return false;
    }
    time_t sec = (time_t)seconds;
    return localtime_s(out, &sec) == 0;
}

static bool seconds_to_filetime(int64_t seconds, FILETIME *out)
{
    if (!out) {
        return false;
    }
    const int64_t epoch = NB_UNIX_EPOCH_FILETIME;
    if (seconds > (INT64_MAX - epoch) / INT64_C(10000000)) {
        return false;
    }
    if (seconds < -epoch / INT64_C(10000000)) {
        return false;
    }
    int64_t value = seconds * INT64_C(10000000) + epoch;
    ULARGE_INTEGER ticks;
    ticks.QuadPart = (uint64_t)value;
    out->dwLowDateTime = ticks.LowPart;
    out->dwHighDateTime = ticks.HighPart;
    return true;
}

bool nb_platform_local_offset_seconds(int64_t seconds, int32_t *out)
{
    if (!out) {
        return false;
    }
    FILETIME utc_filetime;
    if (!seconds_to_filetime(seconds, &utc_filetime)) {
        return false;
    }
    SYSTEMTIME utc_time;
    if (!FileTimeToSystemTime(&utc_filetime, &utc_time)) {
        return false;
    }
    SYSTEMTIME local_time;
    if (!SystemTimeToTzSpecificLocalTime(NULL, &utc_time, &local_time)) {
        return false;
    }
    FILETIME local_filetime;
    if (!SystemTimeToFileTime(&local_time, &local_filetime)) {
        return false;
    }

    ULARGE_INTEGER utc_value;
    utc_value.LowPart = utc_filetime.dwLowDateTime;
    utc_value.HighPart = utc_filetime.dwHighDateTime;
    ULARGE_INTEGER local_value;
    local_value.LowPart = local_filetime.dwLowDateTime;
    local_value.HighPart = local_filetime.dwHighDateTime;
    int64_t delta = (int64_t)local_value.QuadPart -
                    (int64_t)utc_value.QuadPart;
    int64_t offset = delta / INT64_C(10000000);
    if (offset < INT32_MIN || offset > INT32_MAX) {
        return false;
    }
    *out = (int32_t)offset;
    return true;
}

/* Convert days since 1970-01-01 to a civil date. This avoids msvcrt's
 * gmtime() failing for negative time_t values. */
static void civil_from_days(int64_t z, int *year, unsigned *month,
                            unsigned *day)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int y = (int)yoe + (int)era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
    unsigned m = mp + (mp < 10u ? 3u : (unsigned)-9);
    y += (m <= 2u);
    *year = y;
    *month = m;
    *day = d;
}

bool nb_platform_gmtime(int64_t seconds, struct tm *out)
{
    if (!out) {
        return false;
    }
    int64_t days = seconds / INT64_C(86400);
    int64_t remainder = seconds % INT64_C(86400);
    if (remainder < 0) {
        remainder += INT64_C(86400);
        days--;
    }

    int year;
    unsigned month;
    unsigned day;
    civil_from_days(days, &year, &month, &day);

    memset(out, 0, sizeof(*out));
    out->tm_year = year - 1900;
    out->tm_mon = (int)month - 1;
    out->tm_mday = (int)day;
    out->tm_hour = (int)(remainder / 3600);
    out->tm_min = (int)((remainder % 3600) / 60);
    out->tm_sec = (int)(remainder % 60);
    out->tm_wday = (int)((days + 4) % 7);
    if (out->tm_wday < 0) {
        out->tm_wday += 7;
    }
    return true;
}
