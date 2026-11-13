#include "nb/nbase.h"

#include <stdint.h>
#include <stdio.h>

int main(void)
{
    nb_buf *buf = nb_buf_new();
    if (!buf) {
        return 1;
    }

    bool ok = nb_buf_append_cstr(buf, "value=") &&
              nb_buf_append_i64(buf, INT64_MIN) &&
              nb_buf_append_cstr(buf, " max=") &&
              nb_buf_append_u64(buf, UINT64_MAX);
    if (!ok) {
        nb_buf_free(buf);
        return 1;
    }
    printf("buf: %s (len=%zu)\n", nb_buf_cstr(buf), nb_buf_len(buf));

    nb_buf_truncate(buf, 6);
    printf("after truncate: %s (len=%zu)\n", nb_buf_cstr(buf),
           nb_buf_len(buf));

    nb_buf_free(buf);
    return 0;
}
