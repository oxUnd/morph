#include "platform/nb_platform.h"

#include <errno.h>
#include <time.h>

static int64_t timespec_ns(const struct timespec *ts)
{
    return (int64_t)ts->tv_sec * INT64_C(1000000000) + (int64_t)ts->tv_nsec;
}

int64_t nb_platform_time_realtime_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return timespec_ns(&ts);
}

int64_t nb_platform_time_monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return timespec_ns(&ts);
}

void nb_platform_sleep_ms(uint32_t ms)
{
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}

bool nb_platform_localtime(int64_t seconds, struct tm *out)
{
    if (!out) {
        return false;
    }
    time_t sec = (time_t)seconds;
    return localtime_r(&sec, out) != NULL;
}

bool nb_platform_local_offset_seconds(int64_t seconds, int32_t *out)
{
    if (!out) {
        return false;
    }
    time_t sec = (time_t)seconds;
    struct tm tmv;
    if (!localtime_r(&sec, &tmv)) {
        return false;
    }
#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__) || \
    defined(__NetBSD__) || defined(__OpenBSD__)
    if (tmv.tm_gmtoff < INT32_MIN || tmv.tm_gmtoff > INT32_MAX) {
        return false;
    }
    *out = (int32_t)tmv.tm_gmtoff;
#else
    *out = 0;
#endif
    return true;
}

bool nb_platform_gmtime(int64_t seconds, struct tm *out)
{
    if (!out) {
        return false;
    }
    time_t sec = (time_t)seconds;
    return gmtime_r(&sec, out) != NULL;
}
