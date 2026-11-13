#include "nb/nbase.h"

#include <inttypes.h>

/* External library calls and an observed checksum keep the work live without
 * putting volatile stores in the hot loop. Build without LTO for comparisons. */
static volatile uint64_t observed;

typedef struct bench_input {
    nb_str text;
    nb_buf *out;
} bench_input;

typedef uint64_t (*bench_operation)(bench_input *input);

static void require(bool condition)
{
    if (!condition) {
        fputs("text benchmark operation failed\n", stderr);
        exit(1);
    }
}

static uint64_t output_checksum(const nb_buf *out)
{
    return nb_buf_len(out) + (unsigned char)nb_buf_cstr(out)[0];
}

static uint64_t decode_base64(bench_input *input)
{
    nb_buf_clear(input->out);
    require(nb_base64_decode(input->text, NB_B64_STD, input->out, NULL) == NB_OK);
    return output_checksum(input->out);
}

static uint64_t encode_base64(bench_input *input)
{
    nb_buf_clear(input->out);
    require(nb_base64_encode(input->text.ptr, input->text.len, NB_B64_STD,
                             true, input->out, NULL) == NB_OK);
    return output_checksum(input->out);
}

static uint64_t validate_base64(bench_input *input)
{
    bool valid = nb_base64_valid(input->text, NB_B64_STD);
    require(valid);
    return valid;
}

static uint64_t decode_hex(bench_input *input)
{
    nb_buf_clear(input->out);
    require(nb_hex_decode(input->text, input->out, NULL) == NB_OK);
    return output_checksum(input->out);
}

static uint64_t encode_hex(bench_input *input)
{
    nb_buf_clear(input->out);
    require(nb_hex_encode(input->text.ptr, input->text.len, false,
                          input->out, NULL) == NB_OK);
    return output_checksum(input->out);
}

static uint64_t crc32(bench_input *input)
{
    return nb_crc32(input->text.ptr, input->text.len);
}

static uint64_t validate_utf8(bench_input *input)
{
    size_t offset;
    require(nb_utf8_validate(input->text, &offset) == NB_UTF8_OK);
    require(offset == input->text.len);
    return offset;
}

static void measure(const char *name, nb_str text, nb_buf *out,
                     bench_operation operation, size_t iterations)
{
    bench_input input = { text, out };
    (void)operation(&input);
    uint64_t checksum = 0;
    int64_t start = nb_time_monotonic_ns();
    for (size_t i = 0; i < iterations; i++) {
        checksum += operation(&input);
    }
    double ns = (double)(nb_time_monotonic_ns() - start) / (double)iterations;
    observed = checksum;
    printf("%s,%zu,%zu,%.3f,%.6f,%" PRIu64 "\n", name, text.len,
           iterations, ns, (double)text.len / ns, checksum);
}

int main(int argc, char **argv)
{
    uint64_t requested = 65536u;
    if (argc > 3 || (argc > 1 && !nb_str_to_u64(nb_str_from_cstr(argv[1]), &requested)) ||
        requested == 0 || requested > 64u * 1024u * 1024u) {
        fputs("usage: nbase_bench_text [bytes: 1..67108864] [iterations]\n", stderr);
        return 1;
    }
    size_t len = (size_t)requested;
    requested = NB_CLAMP(32u * 1024u * 1024u / len, 8u, 1000000u);
    if (argc > 2 && (!nb_str_to_u64(nb_str_from_cstr(argv[2]), &requested) ||
                     requested == 0 || requested > SIZE_MAX)) {
        fputs("invalid iteration count\n", stderr);
        return 1;
    }
    size_t iterations = (size_t)requested;
    char *random = malloc(len);
    char *ramp = malloc(len);
    char *ascii = malloc(len);
    char *chinese = malloc(len);
    char *mixed = malloc(len);
    require(random && ramp && ascii && chinese && mixed);
    uint32_t state = UINT32_C(1234567);
    for (size_t i = 0; i < len; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        random[i] = (char)state;
        ramp[i] = (char)i;
    }
    memset(ascii, 'a', len);
    memset(chinese, 'a', len);
    memset(mixed, 'a', len);
    for (size_t i = 0; len - i >= 3u; i += 3u) {
        memcpy(chinese + i, "\xe4\xb8\xad", 3u);
    }
    for (size_t i = 0; len - i >= 12u; i += 12u) {
        memcpy(mixed + i, "abc\xe4\xb8\xad\xe6\x96\x87xyz", 12u);
    }
    nb_buf encoded, encoded_ramp, wrapped, hex, out;
    nb_buf_init(&encoded);
    nb_buf_init(&encoded_ramp);
    nb_buf_init(&wrapped);
    nb_buf_init(&hex);
    nb_buf_init(&out);
    require(nb_base64_encode(random, len, NB_B64_STD, true, &encoded, NULL) == NB_OK);
    require(nb_base64_encode(ramp, len, NB_B64_STD, true, &encoded_ramp, NULL) == NB_OK);
    require(nb_hex_encode(random, len, false, &hex, NULL) == NB_OK);
    for (size_t i = 0; i < encoded.len; i++) {
        require(nb_buf_append_char(&wrapped, encoded.ptr[i]));
        if (i % 76u == 75u) {
            require(nb_buf_append_cstr(&wrapped, "\r\n"));
        }
    }
    require(nb_buf_reserve(&out, hex.len));
    puts("scenario,input_bytes,iterations,ns_per_call,GB_per_s,checksum");
    measure("base64_decode_random", nb_buf_str(&encoded), &out, decode_base64, iterations);
    require(nb_buf_len(&out) == len && memcmp(nb_buf_cstr(&out), random, len) == 0);
    measure("base64_decode_ramp", nb_buf_str(&encoded_ramp), &out, decode_base64, iterations);
    require(nb_buf_len(&out) == len && memcmp(nb_buf_cstr(&out), ramp, len) == 0);
    measure("base64_decode_wrapped", nb_buf_str(&wrapped), &out, decode_base64, iterations);
    require(nb_buf_len(&out) == len && memcmp(nb_buf_cstr(&out), random, len) == 0);
    measure("base64_valid_random", nb_buf_str(&encoded), &out, validate_base64, iterations);
    measure("hex_decode_random", nb_buf_str(&hex), &out, decode_hex, iterations);
    require(nb_buf_len(&out) == len && memcmp(nb_buf_cstr(&out), random, len) == 0);
    measure("crc32_random", nb_str_from_parts(random, len), &out, crc32, iterations);
    measure("utf8_ascii", nb_str_from_parts(ascii, len), &out, validate_utf8, iterations);
    measure("utf8_chinese", nb_str_from_parts(chinese, len), &out, validate_utf8, iterations);
    measure("utf8_mixed", nb_str_from_parts(mixed, len), &out, validate_utf8, iterations);
    measure("base64_encode_random", nb_str_from_parts(random, len), &out, encode_base64, iterations);
    require(nb_str_eq(nb_buf_str(&out), nb_buf_str(&encoded)));
    measure("hex_encode_random", nb_str_from_parts(random, len), &out, encode_hex, iterations);
    require(nb_str_eq(nb_buf_str(&out), nb_buf_str(&hex)));
    nb_buf_destroy(&encoded);
    nb_buf_destroy(&encoded_ramp);
    nb_buf_destroy(&wrapped);
    nb_buf_destroy(&hex);
    nb_buf_destroy(&out);
    free(random);
    free(ramp);
    free(ascii);
    free(chinese);
    free(mixed);
    return 0;
}
