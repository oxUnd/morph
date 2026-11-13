/*
 * A small log indexer that exercises every data structure in nbase:
 *
 *   nb_ini   - configuration document
 *   nb_arena - per-run copies of message/source strings and level stats
 *   nb_pool  - fixed-size record objects
 *   nb_vec   - insertion-ordered unique records
 *   nb_map   - message deduplication and per-level counters
 *   nb_buf   - input log, serialized report, temp path, read-back
 *   nb_str   - borrowed line/token views and trimming
 *
 * It also uses the supporting modules: hash (CRC-32 fingerprint), encoding
 * (hex), utf8 (validation), time (RFC 3339), log (counting sink), err and
 * file (temp-file round trip).
 */
#include "nb/nbase.h"

#include <stdio.h>
#include <string.h>

typedef struct record {
    nb_str message;
    nb_str source;
    int64_t timestamp_ns;
    nb_log_level level;
    uint32_t fingerprint;
    size_t occurrences;
} record;

typedef struct level_stat {
    nb_str name;
    size_t count;
} level_stat;

typedef struct case_state {
    nb_ini *config;
    nb_buf *input;
    nb_buf *report;
    nb_buf *temp_path;
    nb_buf *roundtrip;
    nb_arena *arena;
    nb_pool *pool;
    nb_map *by_message;
    nb_map *by_level;
    nb_vec records;
    nb_log *log;
    size_t log_records;
    size_t lines;
    size_t accepted;
    size_t duplicates;
    size_t skipped;
    size_t invalid;
} case_state;

static const char config_text[] =
    "[index]\n"
    "max_records = 64\n"
    "min_level = INFO\n"
    "[output]\n"
    "title = \"Telemetry index\"\n";

static const char input_text[] =
    "2026-11-10T08:00:00Z INFO boot started\n"
    "2026-11-10T08:00:01Z WARN disk slow io\n"
    "2026-11-10T08:00:02Z ERROR net timeout\n"
    "2026-11-10T08:00:03Z INFO boot started\n"
    "2026-11-10T08:00:04Z DEBUG cache hit\n"
    "2026-11-10T08:00:05Z INFO utf8 caf\xC3\xA9\n"
    "2026-11-10T08:00:06Z INFO bad \xC3\x28\n"
    "\n";

static void count_sink(void *ctx, nb_log_level level,
                       const char *record_text, size_t len)
{
    (void)level;
    (void)record_text;
    (void)len;
    ((case_state *)ctx)->log_records++;
}

static int level_rank(nb_str name)
{
    if (nb_str_eq_icase(name, NB_STR_LIT("TRACE"))) {
        return NB_LOG_TRACE;
    }
    if (nb_str_eq_icase(name, NB_STR_LIT("DEBUG"))) {
        return NB_LOG_DEBUG;
    }
    if (nb_str_eq_icase(name, NB_STR_LIT("INFO"))) {
        return NB_LOG_INFO;
    }
    if (nb_str_eq_icase(name, NB_STR_LIT("WARN"))) {
        return NB_LOG_WARN;
    }
    if (nb_str_eq_icase(name, NB_STR_LIT("ERROR"))) {
        return NB_LOG_ERROR;
    }
    if (nb_str_eq_icase(name, NB_STR_LIT("FATAL"))) {
        return NB_LOG_FATAL;
    }
    return -1;
}

static bool parse_line(nb_str line, nb_str *timestamp, nb_str *level,
                       nb_str *source, nb_str *message)
{
    nb_split_iter fields;
    nb_split_init(&fields, line, ' ');
    nb_split_skip_empty(&fields, true);
    if (!nb_split_next(&fields, timestamp) || timestamp->len == 0) {
        return false;
    }
    if (!nb_split_next(&fields, level) || level->len == 0) {
        return false;
    }
    if (!nb_split_next(&fields, source) || source->len == 0) {
        return false;
    }
    *message = nb_str_trim(fields.rest);
    return message->len != 0;
}

/* Consume one line. nb_buf_consume only advances the read cursor, so the
 * returned view stays valid until the next append/compact. */
static bool next_line(nb_buf *input, nb_str *line)
{
    if (nb_buf_len(input) == 0) {
        return false;
    }
    nb_str visible = nb_buf_str(input);
    size_t newline = nb_str_find_char(visible, '\n');
    if (newline == SIZE_MAX) {
        *line = nb_str_trim(visible);
        input->off = input->len;
        return true;
    }
    *line = nb_str_trim(nb_str_from_parts(visible.ptr, newline));
    nb_buf_consume(input, newline + 1u);
    return true;
}

static int case_fail(const char *what, const nb_err *err)
{
    if (err && err->msg[0] != '\0') {
        fprintf(stderr, "%s: %s\n", what, err->msg);
    } else {
        fprintf(stderr, "%s\n", what);
    }
    return 1;
}

static void case_cleanup(case_state *state)
{
    nb_map_free(state->by_message);
    nb_map_free(state->by_level);
    nb_vec_destroy(&state->records);
    nb_pool_free(state->pool);
    nb_arena_free(state->arena);
    nb_log_free(state->log);
    nb_ini_free(state->config);
    nb_buf_free(state->roundtrip);
    nb_buf_free(state->temp_path);
    nb_buf_free(state->report);
    nb_buf_free(state->input);
}

static bool case_init(case_state *state)
{
    nb_err err;
    nb_err_clear(&err);

    state->config = nb_ini_from_string(nb_str_from_cstr(config_text), &err);
    if (!state->config) {
        fprintf(stderr, "config parse failed: %s\n", err.msg);
        return false;
    }

    state->input = nb_buf_new();
    state->report = nb_buf_new();
    state->temp_path = nb_buf_new();
    state->roundtrip = nb_buf_new();
    state->arena = nb_arena_new(4096);
    state->pool = nb_pool_new(sizeof(record), 8);
    state->by_message = nb_map_new();
    state->by_level = nb_map_new();
    state->log = nb_log_new(NB_LOG_DEBUG, count_sink, state);
    nb_vec_init(&state->records, sizeof(record *));

    if (!state->input || !state->report || !state->temp_path ||
        !state->roundtrip || !state->arena || !state->pool ||
        !state->by_message || !state->by_level || !state->log) {
        fprintf(stderr, "failed to allocate example state\n");
        return false;
    }
    if (!nb_buf_append_cstr(state->input, input_text)) {
        fprintf(stderr, "failed to build input buffer\n");
        return false;
    }
    return true;
}

static int case_run(case_state *state)
{
    nb_err err;
    nb_err_clear(&err);

    nb_str title = nb_ini_get_str(state->config, "output", "title");
    if (!title.ptr) {
        title = NB_STR_LIT("log index");
    }
    nb_str min_name = nb_ini_get_str(state->config, "index", "min_level");
    if (!min_name.ptr) {
        min_name = NB_STR_LIT("INFO");
    }
    int minimum = level_rank(min_name);
    if (minimum < 0) {
        minimum = NB_LOG_INFO;
    }
    int64_t configured_max =
        nb_ini_get_int(state->config, "index", "max_records", 64);
    size_t max_records = configured_max > 0 ? (size_t)configured_max : 64u;

    nb_str line;
    while (next_line(state->input, &line)) {
        if (line.len == 0) {
            continue;
        }
        state->lines++;

        nb_str timestamp_text;
        nb_str level_text;
        nb_str source_text;
        nb_str message_text;
        if (!parse_line(line, &timestamp_text, &level_text, &source_text,
                        &message_text)) {
            state->invalid++;
            continue;
        }

        int rank = level_rank(level_text);
        if (rank < 0) {
            state->invalid++;
            continue;
        }
        if (rank < minimum) {
            state->skipped++;
            continue;
        }
        if (!nb_utf8_valid(message_text)) {
            state->invalid++;
            continue;
        }
        if (nb_vec_len(&state->records) >= max_records) {
            state->skipped++;
            continue;
        }

        record *existing = nb_map_get(state->by_message, message_text);
        if (existing) {
            existing->occurrences++;
            state->duplicates++;
            continue;
        }

        int64_t timestamp_ns = 0;
        if (!nb_time_parse_rfc3339_str(timestamp_text, &timestamp_ns)) {
            state->invalid++;
            continue;
        }

        record *item = nb_pool_alloc0(state->pool);
        if (!item) {
            return case_fail("record allocation failed", NULL);
        }
        nb_str message = nb_arena_strdup(state->arena, message_text);
        nb_str source = nb_arena_strdup(state->arena, source_text);
        if (!message.ptr || !source.ptr) {
            nb_pool_release(state->pool, item);
            return case_fail("arena allocation failed", NULL);
        }

        item->message = message;
        item->source = source;
        item->timestamp_ns = timestamp_ns;
        item->level = (nb_log_level)rank;
        item->fingerprint = nb_crc32(message_text.ptr, message_text.len);
        item->occurrences = 1;

        if (!nb_map_put(state->by_message, message_text, item, NULL)) {
            nb_pool_release(state->pool, item);
            return case_fail("message index insert failed", NULL);
        }
        if (!nb_vec_push_copy(&state->records, &item)) {
            nb_map_del(state->by_message, message_text);
            nb_pool_release(state->pool, item);
            return case_fail("record vector append failed", NULL);
        }
        state->accepted++;

        void **level_slot = nb_map_slot(state->by_level, level_text, NULL);
        if (!level_slot) {
            return case_fail("level index slot failed", NULL);
        }
        level_stat *stat = *level_slot;
        if (!stat) {
            stat = nb_arena_alloc0(state->arena, sizeof(*stat));
            if (!stat) {
                return case_fail("level stat allocation failed", NULL);
            }
            stat->name = nb_str_from_cstr(nb_log_level_str(rank));
            stat->count = 0;
            *level_slot = stat;
        }
        stat->count++;

        nb_log_write(state->log, NB_LOG_DEBUG,
                     "indexed %.*s from %.*s", (int)source.len, source.ptr,
                     (int)message.len, message.ptr);
    }

    if (!nb_buf_printf(state->report, "== %.*s ==\n", (int)title.len,
                       title.ptr) ||
        !nb_buf_printf(state->report,
                       "lines=%zu unique=%zu duplicates=%zu skipped=%zu "
                       "invalid=%zu\n",
                       state->lines, state->accepted, state->duplicates,
                       state->skipped, state->invalid) ||
        !nb_buf_printf(
            state->report,
            "arena_used=%zu peak=%zu cached=%zu pool_used=%zu/%zu "
            "peak=%zu cached=%zu map_size=%zu\n",
            nb_arena_used(state->arena), nb_arena_peak(state->arena),
            nb_arena_cached(state->arena), nb_pool_used(state->pool),
            nb_pool_capacity(state->pool), nb_pool_peak(state->pool),
            nb_pool_cached(state->pool), nb_map_size(state->by_message))) {
        return case_fail("report header failed", NULL);
    }

    if (!nb_buf_append_cstr(state->report, "-- levels --\n")) {
        return case_fail("report levels header failed", NULL);
    }
    nb_map_iter level_iter;
    nb_str level_key;
    void *level_value;
    nb_map_iter_init(state->by_level, &level_iter);
    while (nb_map_iter_next(&level_iter, &level_key, &level_value)) {
        level_stat *stat = level_value;
        if (!nb_buf_printf(state->report, "  %-5.*s %zu\n",
                           (int)level_key.len, level_key.ptr, stat->count)) {
            return case_fail("report level row failed", NULL);
        }
    }

    if (!nb_buf_append_cstr(state->report, "-- records --\n")) {
        return case_fail("report records header failed", NULL);
    }
    for (size_t i = 0; i < nb_vec_len(&state->records); i++) {
        record *item = *(record **)nb_vec_at(&state->records, i);
        char timestamp_buf[32];
        if (!nb_time_format_rfc3339_utc(item->timestamp_ns, timestamp_buf,
                                        sizeof(timestamp_buf))) {
            return case_fail("timestamp format failed", NULL);
        }
        unsigned char digest[4];
        digest[0] = (unsigned char)(item->fingerprint >> 24);
        digest[1] = (unsigned char)(item->fingerprint >> 16);
        digest[2] = (unsigned char)(item->fingerprint >> 8);
        digest[3] = (unsigned char)item->fingerprint;
        char fingerprint_hex[9];
        nb_err_clear(&err);
        if (nb_hex_encode_to(digest, sizeof(digest), true, fingerprint_hex,
                             sizeof(fingerprint_hex), &err) != NB_OK) {
            return case_fail("fingerprint encoding failed", &err);
        }
        if (!nb_buf_printf(state->report,
                           "%s %-5s %.*s x%zu fp=%s msg=%.*s\n",
                           timestamp_buf, nb_log_level_str(item->level),
                           (int)item->source.len, item->source.ptr,
                           item->occurrences, fingerprint_hex,
                           (int)item->message.len, item->message.ptr)) {
            return case_fail("report record failed", NULL);
        }
    }

    if (!nb_buf_ok(state->report)) {
        return case_fail("report construction failed", NULL);
    }

    nb_err_clear(&err);
    if (nb_file_temp(state->temp_path, &err) != NB_OK) {
        return case_fail("temp path failed", &err);
    }
    if (nb_file_write_all(nb_buf_cstr(state->temp_path),
                          nb_buf_cstr(state->report),
                          nb_buf_len(state->report), &err) != NB_OK) {
        return case_fail("temp write failed", &err);
    }
    if (nb_file_read_all(nb_buf_cstr(state->temp_path), state->roundtrip, 0, &err) != NB_OK) {
        return case_fail("temp read failed", &err);
    }
    if (nb_buf_len(state->roundtrip) != nb_buf_len(state->report) ||
        memcmp(nb_buf_cstr(state->roundtrip), nb_buf_cstr(state->report),
               nb_buf_len(state->report)) != 0) {
        return case_fail("temp round trip mismatch", NULL);
    }
    if (nb_file_remove(nb_buf_cstr(state->temp_path), &err) != NB_OK) {
        return case_fail("temp remove failed", &err);
    }

    fwrite(nb_buf_cstr(state->report), 1, nb_buf_len(state->report), stdout);
    nb_log_write(state->log, NB_LOG_INFO,
                 "indexed %zu unique records (%zu duplicates)",
                 state->accepted, state->duplicates);

    if (state->accepted == 0 || state->duplicates == 0 ||
        state->skipped == 0 || state->invalid == 0 ||
        state->log_records == 0) {
        return case_fail("unexpected example counters", NULL);
    }

    nb_buf_compact(state->input);
    nb_arena_reset(state->arena);
    nb_pool_reset(state->pool);
    nb_map_clear(state->by_message);
    nb_map_clear(state->by_level);
    nb_vec_clear(&state->records);
    printf("reset: arena_used=%zu pool_used=%zu records=%zu log_records=%zu\n",
           nb_arena_used(state->arena), nb_pool_used(state->pool),
           nb_vec_len(&state->records), state->log_records);
    return 0;
}

int main(void)
{
    case_state state;
    memset(&state, 0, sizeof(state));
    nb_vec_init(&state.records, sizeof(record *));

    int status = 1;
    if (case_init(&state)) {
        status = case_run(&state);
    }
    case_cleanup(&state);
    return status;
}
