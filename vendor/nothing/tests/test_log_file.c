#include "test.h"

#include "nb/file.h"
#include "nb/log.h"

static void write_records(nb_log_file *file, size_t count, size_t payload)
{
    char record[256];
    for (size_t i = 0; i < count; i++) {
        memset(record, (int)('a' + (char)(i % 26u)), payload);
        record[payload] = '\n';
        nb_log_file_sink(file, NB_LOG_INFO, record, payload + 1u);
    }
}

static bool has_suffix(const char *path, const char *suffix)
{
    nb_buf *candidate = nb_buf_new();
    bool exists = false;
    if (candidate && nb_buf_append_cstr(candidate, path) && nb_buf_append_cstr(candidate, suffix)) {
        exists = nb_file_exists(nb_buf_cstr(candidate));
    }
    nb_buf_free(candidate);
    return exists;
}

void test_log_file(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *dir = nb_buf_new();
    nb_buf *path = nb_buf_new();
    NB_CHECK(dir != NULL && path != NULL);
    if (!dir || !path) {
        nb_buf_free(path);
        nb_buf_free(dir);
        return;
    }
    if (nb_file_temp_dir(dir, NULL, NULL, &err) != NB_OK ||
        nb_path_join(path, nb_buf_cstr(dir), "agent.log") != NB_OK) {
        NB_CHECK(0);
        nb_buf_free(path);
        nb_buf_free(dir);
        return;
    }

    nb_log_file *file = nb_log_file_open(nb_buf_cstr(path), 1024u, 2u);
    NB_CHECK(file != NULL);
    if (file) {
        write_records(file, 12u, 200u);
        nb_log_file_close(file);
    }
    NB_CHECK(nb_file_exists(nb_buf_cstr(path)));
    NB_CHECK(has_suffix(nb_buf_cstr(path), ".1"));
    NB_CHECK(has_suffix(nb_buf_cstr(path), ".2"));
    NB_CHECK(!has_suffix(nb_buf_cstr(path), ".3"));

    /* A zero budget disables rotation regardless of max_files. */
    file = nb_log_file_open(nb_buf_cstr(path), 0u, 0u);
    NB_CHECK(file != NULL);
    if (file) {
        nb_log *logger = nb_log_new(NB_LOG_INFO, nb_log_file_sink, file);
        NB_CHECK(logger != NULL);
        if (logger) {
            nb_log_write(logger, NB_LOG_INFO, "integration %d", 7);
            nb_log_free(logger);
        }
        nb_log_file_close(file);
    }

    nb_log *owned = nb_log_new_file(NB_LOG_DEBUG, nb_buf_cstr(path), 0u, 0u);
    NB_CHECK(owned != NULL);
    if (owned) {
        nb_log_write(owned, NB_LOG_INFO, "owned sink lifetime");
        nb_log_free(owned);
    }
    NB_CHECK(nb_log_new_file(NB_LOG_INFO, NULL, 0u, 0u) == NULL);
    NB_CHECK(nb_log_new_file(NB_LOG_INFO, "/missing/nbase/log", 0u, 0u) == NULL);
    NB_CHECK(nb_log_file_open(NULL, 0u, 0u) == NULL);
    NB_CHECK(nb_log_file_open(nb_buf_cstr(path), 1u, 0u) == NULL);
    NB_CHECK(nb_file_remove_all(nb_buf_cstr(dir), 0, &err) == NB_OK);
    nb_buf_free(path);
    nb_buf_free(dir);
}
