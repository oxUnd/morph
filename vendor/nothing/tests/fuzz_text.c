#include "nb/nbase.h"

static void check_roundtrip(nb_str input, unsigned operation)
{
    nb_buf encoded;
    nb_buf decoded;
    nb_buf_init(&encoded);
    nb_buf_init(&decoded);
    nb_status status;
    if (operation == 0u) {
        status = nb_hex_encode(input.ptr, input.len, false, &encoded, NULL);
        if (status == NB_OK) {
            status = nb_hex_decode(nb_buf_str(&encoded), &decoded, NULL);
        }
    } else if (operation == 1u) {
        status = nb_base64_encode(input.ptr, input.len, NB_B64_STD, true,
                                  &encoded, NULL);
        if (status == NB_OK) {
            status = nb_base64_decode(nb_buf_str(&encoded), NB_B64_STD,
                                      &decoded, NULL);
        }
    } else {
        status = nb_url_encode(input, true, &encoded, NULL);
        if (status == NB_OK) {
            status = nb_url_decode(nb_buf_str(&encoded), true, &decoded, NULL);
        }
    }
    NB_ASSERT(status == NB_OK);
    NB_ASSERT(nb_str_eq(input, nb_buf_str(&decoded)));
    nb_buf_destroy(&encoded);
    nb_buf_destroy(&decoded);
}

static void check_utf8_validation(nb_str input)
{
    size_t expected_offset = 0;
    nb_utf8_status expected = NB_UTF8_OK;
    while (expected_offset < input.len) {
        size_t next = expected_offset;
        uint32_t cp;
        if (!nb_utf8_decode(input, &next, &cp, &expected)) {
            break;
        }
        expected_offset = next;
    }
    size_t offset = SIZE_MAX;
    NB_ASSERT(nb_utf8_validate(input, &offset) == expected);
    NB_ASSERT(offset == expected_offset);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    nb_str input = nb_str_from_parts((const char *)data, size);
    nb_buf out;
    nb_buf_init(&out);
    (void)nb_hex_decode(input, &out, NULL);
    nb_buf_clear(&out);
    (void)nb_base64_decode(input, NB_B64_STD, &out, NULL);
    nb_buf_clear(&out);
    (void)nb_base64_decode(input, NB_B64_URL, &out, NULL);
    nb_buf_clear(&out);
    (void)nb_url_decode(input, true, &out, NULL);
    nb_buf_destroy(&out);
    for (unsigned operation = 0; operation < 3u; operation++) {
        check_roundtrip(input, operation);
    }

    int64_t integer;
    uint64_t unsigned_integer;
    double real;
    (void)nb_str_to_i64(input, &integer);
    (void)nb_str_to_u64(input, &unsigned_integer);
    (void)nb_str_to_f64(input, &real);
    (void)nb_time_parse_rfc3339_str(input, &integer);
    (void)nb_utf8_count(input);
    check_utf8_validation(input);
    (void)nb_utf8_trim(input);
    (void)nb_utf8_slice(input, size / 3u, size / 2u);

    nb_ini *ini = nb_ini_new();
    NB_ASSERT(ini != NULL);
    NB_ASSERT(nb_ini_set(ini, "existing", "key", "old") == NB_OK);
    nb_status status = nb_ini_parse(ini, input, NULL);
    if (status != NB_OK) {
        NB_ASSERT(nb_ini_section_count(ini) == 1u);
        NB_ASSERT(strcmp(nb_ini_get(ini, "existing", "key"), "old") == 0);
    }
    nb_ini_free(ini);
    return 0;
}
