#include "nb/base.h"

void nb_assert_fail(const char *expr, const char *file, int line)
{
    fprintf(stderr, "nbase: assertion failed: %s (%s:%d)\n", expr, file, line);
    fflush(stderr);
    abort();
}

const char *nb_version(void)
{
    return NB_VERSION_STRING;
}
