#include "nb/nbase.h"

#include <stdio.h>

int main(void)
{
    const int64_t sample = INT64_C(1794183240123000000);
    char text[32];
    if (!nb_time_format_rfc3339_utc(sample, text, sizeof(text))) {
        return 1;
    }

    int64_t parsed = 0;
    bool ok = nb_time_parse_rfc3339(text, &parsed);
    printf("utc=%s\n", text);
    printf("roundtrip=%lld ns\n", (long long)(parsed - sample));

    int64_t ignored = 0;
    printf("strict=%d\n",
           nb_time_parse_rfc3339("2026-+1-01T00:00:00Z", &ignored));
    return ok ? 0 : 1;
}
