#include "nb/nbase.h"

#include <stdio.h>

static void stdout_sink(void *ctx, nb_log_level level,
                        const char *record, size_t len)
{
    (void)ctx;
    (void)level;
    fwrite(record, 1, len, stdout);
}

typedef struct counting_sink {
    size_t records;
} counting_sink;

static void count_sink(void *ctx, nb_log_level level,
                       const char *record, size_t len)
{
    (void)level;
    (void)record;
    (void)len;
    ((counting_sink *)ctx)->records++;
}

int main(void)
{
    nb_log *log = nb_log_new(NB_LOG_DEBUG, stdout_sink, NULL);
    if (!log) {
        return 1;
    }
    nb_log_write(log, NB_LOG_DEBUG, "debug message");
    nb_log_write(log, NB_LOG_WARN, "value=%d", 42);
    nb_log_set_level(log, NB_LOG_ERROR);
    nb_log_write(log, NB_LOG_WARN, "this line is filtered");
    nb_log_write(log, NB_LOG_ERROR, "only errors remain");

    counting_sink counts = { 0 };
    nb_log_set_sink(log, count_sink, &counts);
    nb_log_write(log, NB_LOG_ERROR, "captured by new sink");
    printf("captured records=%zu\n", counts.records);

    nb_log_free(log);
    return 0;
}
