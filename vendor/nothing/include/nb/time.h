#ifndef NB_TIME_H
#define NB_TIME_H

#include "nb/base.h"
#include "nb/str.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Nanoseconds since the Unix epoch (CLOCK_REALTIME). */
NB_EXPORT int64_t nb_time_now_ns(void);

/* Monotonic nanoseconds, suitable for measuring intervals. */
NB_EXPORT int64_t nb_time_monotonic_ns(void);

NB_EXPORT int64_t nb_time_now_ms(void);
NB_EXPORT int64_t nb_time_monotonic_ms(void);
/* A nonpositive interval expires immediately. Addition saturates at INT64_MAX. */
NB_EXPORT int64_t nb_time_deadline_after_ns(int64_t timeout_ns);
/* Remaining monotonic duration, clamped to zero for an expired deadline. */
NB_EXPORT int64_t nb_time_remaining_ns(int64_t deadline_ns);
NB_EXPORT void nb_time_sleep_ms(uint32_t ms);

/* Formats as RFC 3339 with millisecond precision and the local UTC offset,
 * for example 2026-11-09T08:14:00.123+08:00. */
NB_EXPORT bool nb_time_format_rfc3339(int64_t ns, char *buf, size_t cap);

/* Formats as RFC 3339 in UTC with millisecond precision and a trailing 'Z',
 * for example 2026-11-09T00:14:00.123Z. Negative nanosecond values are
 * normalized to the preceding second. Returns false when buf is NULL or cap
 * cannot hold the complete string. */
NB_EXPORT bool nb_time_format_rfc3339_utc(int64_t ns, char *buf, size_t cap);

/* Parses RFC 3339 with a numeric offset or Z.
 *
 * The date and time fields must use their fixed widths and separators. A
 * fractional part must contain at least one digit; digits after the ninth are
 * consumed but ignored. Leap second 60 and lowercase 'z' are accepted. */
NB_EXPORT bool nb_time_parse_rfc3339(const char *text, int64_t *ns_out);
NB_EXPORT bool nb_time_parse_rfc3339_str(nb_str text, int64_t *ns_out);

#ifdef __cplusplus
}
#endif

#endif /* NB_TIME_H */
