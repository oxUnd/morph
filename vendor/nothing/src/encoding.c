#include "nb/encoding.h"

#include "nb/mem.h"
#include "container_internal.h"

#include <string.h>

/* Reserve once after validation; self-input is snapshotted before reserve can
 * relocate or compact out. No fallible operation follows a successful begin. */
static nb_status encoding_begin(nb_buf *out, nb_str input, size_t additional,
                                 char **owned, char **dst, nb_err *err)
{
    *owned = NULL;
    size_t visible = nb_buf_len(out);
    if (additional >= SIZE_MAX - visible) {
        nb_err_set(err, NB_ERR_RANGE, "encoded output is too large");
        return NB_ERR_RANGE;
    }
    size_t offset = 0;
    if (input.len != 0 && nb_span_offset(out->ptr,
                                        out->len == SIZE_MAX ? SIZE_MAX : out->len + 1u,
                                        input.ptr,
                                        input.len, &offset)) {
        *owned = nb_try_alloc(input.len);
        if (!*owned) {
            out->error = true;
            nb_err_set(err, NB_ERR_NOMEM, "out of memory copying encoding input");
            return NB_ERR_NOMEM;
        }
        memcpy(*owned, input.ptr, input.len);
    }
    if (!nb_buf_reserve(out, additional)) {
        nb_free(*owned, input.len);
        *owned = NULL;
        nb_err_set(err, NB_ERR_NOMEM, "out of memory reserving encoded output");
        return NB_ERR_NOMEM;
    }
    *dst = out->ptr + out->len;
    return NB_OK;
}

static void encoding_end(nb_buf *out, size_t written, char *owned, size_t len)
{
    out->len += written;
    out->ptr[out->len] = '\0';
    nb_free(owned, len);
}

static bool is_base64_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Values >= 64 are handled by the grammar, never used as sextets. */
static const unsigned char base64_std_values[256] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,  62, 255, 255, 255,  63,
     52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255, 255, 255, 255, 255,
    255,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
     15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255, 255,
    255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
     41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

static const unsigned char base64_url_values[256] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,  62, 255, 255,
     52,  53,  54,  55,  56,  57,  58,  59,  60,  61, 255, 255, 255, 255, 255, 255,
    255,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,
     15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25, 255, 255, 255, 255,  63,
    255,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
     41,  42,  43,  44,  45,  46,  47,  48,  49,  50,  51, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

static const unsigned char hex_values[256] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9, 255, 255, 255, 255, 255, 255,
    255,  10,  11,  12,  13,  14,  15, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255,  10,  11,  12,  13,  14,  15, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

/*
 * Single source of truth for base64 acceptance. It does not touch the output
 * buffer, so callers can use it for validation and then decode without
 * repeating the grammar checks.
 */
static bool base64_validate(nb_str input, nb_b64_variant variant,
                             size_t *decoded_len, bool *dense)
{
    if (variant != NB_B64_STD && variant != NB_B64_URL) {
        return false;
    }
    if (input.len != 0 && !input.ptr) {
        return false;
    }

    size_t chars = 0;
    size_t padding = 0;
    unsigned char last = 0;
    bool saw_padding = false;
    bool has_space = false;
    const unsigned char *values = variant == NB_B64_STD
        ? base64_std_values : base64_url_values;
    size_t i = 0;
    while (i < input.len) {
        if (!saw_padding && input.len - i >= 4u) {
            unsigned a = values[(unsigned char)input.ptr[i]];
            unsigned b = values[(unsigned char)input.ptr[i + 1u]];
            unsigned c = values[(unsigned char)input.ptr[i + 2u]];
            unsigned d = values[(unsigned char)input.ptr[i + 3u]];
            if ((a | b | c | d) < 64u) {
                chars += 4u;
                last = (unsigned char)d;
                i += 4u;
                continue;
            }
        }
        unsigned char c = (unsigned char)input.ptr[i++];
        if (is_base64_space(c)) {
            has_space = true;
            continue;
        }
        if (c == '=') {
            if (++padding > 2u) {
                return false;
            }
            saw_padding = true;
            continue;
        }
        if (saw_padding) {
            return false;
        }
        unsigned value = values[c];
        if (value >= 64u) {
            return false;
        }
        last = (unsigned char)value;
        chars++;
    }

    if (padding != 0) {
        if ((chars % 4u + padding) % 4u != 0u) {
            return false;
        }
    } else if (chars % 4u == 1u) {
        return false;
    }

    size_t remainder = chars % 4u;
    if ((remainder == 2u && (last & 0x0fu) != 0u) ||
        (remainder == 3u && (last & 0x03u) != 0u)) {
        return false;
    }
    if (decoded_len) {
        *decoded_len = chars / 4u * 3u + (remainder ? remainder - 1u : 0u);
    }
    if (dense) {
        *dense = !has_space;
    }
    return true;
}

static int hex_value(unsigned char c)
{
    unsigned value = hex_values[c];
    return value < 16u ? (int)value : -1;
}

nb_status nb_base64_encode(const void *data, size_t len,
                           nb_b64_variant variant, bool pad,
                           nb_buf *out, nb_err *err)
{
    if (!out || (len != 0 && !data) ||
        (variant != NB_B64_STD && variant != NB_B64_URL)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid base64 arguments");
        return NB_ERR_INVAL;
    }
    size_t tail = len % 3u ? (pad ? 4u : len % 3u + 1u) : 0u;
    if (len / 3u > (SIZE_MAX - tail) / 4u) {
        nb_err_set(err, NB_ERR_RANGE, "base64 output is too large");
        return NB_ERR_RANGE;
    }
    size_t count = len / 3u * 4u + tail;
    char *owned;
    char *dst;
    nb_status status = encoding_begin(out, nb_str_from_parts(data, len),
                                      count, &owned, &dst, err);
    if (status != NB_OK) {
        return status;
    }
    const unsigned char *src = (const unsigned char *)(owned ? owned : data);
    const char *alphabet = variant == NB_B64_STD
        ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
        : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0;
    size_t j = 0;
    while (len - i >= 3u) {
        uint32_t v = ((uint32_t)src[i] << 16) |
                     ((uint32_t)src[i + 1u] << 8) | src[i + 2u];
        dst[j++] = alphabet[(v >> 18) & 0x3fu];
        dst[j++] = alphabet[(v >> 12) & 0x3fu];
        dst[j++] = alphabet[(v >> 6) & 0x3fu];
        dst[j++] = alphabet[v & 0x3fu];
        i += 3u;
    }
    if (i < len) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (len - i == 2u) {
            v |= (uint32_t)src[i + 1u] << 8;
        }
        dst[j++] = alphabet[(v >> 18) & 0x3fu];
        dst[j++] = alphabet[(v >> 12) & 0x3fu];
        if (len - i == 2u) {
            dst[j++] = alphabet[(v >> 6) & 0x3fu];
        }
        while (j < count) {
            dst[j++] = '=';
        }
    }
    encoding_end(out, count, owned, len);
    return NB_OK;
}

bool nb_base64_valid(nb_str input, nb_b64_variant variant)
{
    return base64_validate(input, variant, NULL, NULL);
}

nb_status nb_base64_decode(nb_str input, nb_b64_variant variant,
                           nb_buf *out, nb_err *err)
{
    if (!out || (input.len != 0 && !input.ptr) ||
        (variant != NB_B64_STD && variant != NB_B64_URL)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid base64 arguments");
        return NB_ERR_INVAL;
    }
    size_t count = 0;
    bool dense = false;
    if (!base64_validate(input, variant, &count, &dense)) {
        nb_err_set(err, NB_ERR_PARSE, "invalid base64 input");
        return NB_ERR_PARSE;
    }
    char *owned;
    char *dst;
    nb_status status = encoding_begin(out, input, count, &owned, &dst, err);
    if (status != NB_OK) {
        return status;
    }
    const char *src = owned ? owned : input.ptr;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    size_t j = 0;
    size_t i = 0;
    const unsigned char *values = variant == NB_B64_STD
        ? base64_std_values : base64_url_values;
    /* Validation proved these quartets contain no whitespace or padding.
     * Keep the fallible reserve after validation to preserve parse errors,
     * the read cursor, and self-input on every failure path. */
    if (dense) {
        size_t groups = count / 3u;
        for (size_t group = 0; group < groups; group++) {
            uint32_t word = ((uint32_t)values[(unsigned char)src[i]] << 18) |
                            ((uint32_t)values[(unsigned char)src[i + 1u]] << 12) |
                            ((uint32_t)values[(unsigned char)src[i + 2u]] << 6) |
                            values[(unsigned char)src[i + 3u]];
            dst[j] = (char)(word >> 16);
            dst[j + 1u] = (char)(word >> 8);
            dst[j + 2u] = (char)word;
            i += 4u;
            j += 3u;
        }
    }
    for (; i < input.len; i++) {
        unsigned value = values[(unsigned char)src[i]];
        if (value >= 64u) {
            continue;
        }
        accumulator = (accumulator << 6) | (uint32_t)value;
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            dst[j++] = (char)((accumulator >> bits) & 0xffu);
        }
    }
    encoding_end(out, count, owned, input.len);
    return NB_OK;
}

nb_status nb_hex_encode(const void *data, size_t len, bool uppercase,
                        nb_buf *out, nb_err *err)
{
    if (!out || (len != 0 && !data)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid hex arguments");
        return NB_ERR_INVAL;
    }
    if (len > SIZE_MAX / 2u) {
        nb_err_set(err, NB_ERR_RANGE, "hex output is too large");
        return NB_ERR_RANGE;
    }
    char *owned;
    char *dst;
    nb_status status = encoding_begin(out, nb_str_from_parts(data, len),
                                      len * 2u, &owned, &dst, err);
    if (status != NB_OK) {
        return status;
    }
    const unsigned char *src = (const unsigned char *)(owned ? owned : data);
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[i * 2u] = digits[src[i] >> 4];
        dst[i * 2u + 1u] = digits[src[i] & 0x0fu];
    }
    encoding_end(out, len * 2u, owned, len);
    return NB_OK;
}

nb_status nb_hex_encode_to(const void *data, size_t len, bool uppercase,
                           char *out, size_t cap, nb_err *err)
{
    if ((len != 0 && !data) || !out) {
        nb_err_set(err, NB_ERR_INVAL, "invalid hex arguments");
        return NB_ERR_INVAL;
    }
    if (len > (SIZE_MAX - 1u) / 2u || cap < len * 2u + 1u) {
        nb_err_set(err, NB_ERR_RANGE, "hex output buffer is too small");
        return NB_ERR_RANGE;
    }
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    const unsigned char *src = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        out[i * 2u] = digits[src[i] >> 4];
        out[i * 2u + 1u] = digits[src[i] & 0x0fu];
    }
    out[len * 2u] = '\0';
    return NB_OK;
}

nb_status nb_hex_decode(nb_str input, nb_buf *out, nb_err *err)
{
    if (!out || (input.len != 0 && !input.ptr)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid hex arguments");
        return NB_ERR_INVAL;
    }
    if (input.len % 2u != 0u) {
        nb_err_set(err, NB_ERR_PARSE, "hex input has odd length");
        return NB_ERR_PARSE;
    }
    for (size_t i = 0; i < input.len; i++) {
        if (hex_values[(unsigned char)input.ptr[i]] >= 16u) {
            nb_err_set(err, NB_ERR_PARSE, "invalid hex character");
            return NB_ERR_PARSE;
        }
    }
    char *owned;
    char *dst;
    nb_status status = encoding_begin(out, input, input.len / 2u,
                                      &owned, &dst, err);
    if (status != NB_OK) {
        return status;
    }
    const char *src = owned ? owned : input.ptr;
    for (size_t i = 0; i < input.len; i += 2u) {
        unsigned hi = hex_values[(unsigned char)src[i]];
        unsigned lo = hex_values[(unsigned char)src[i + 1u]];
        dst[i / 2u] = (char)((hi << 4) | lo);
    }
    encoding_end(out, input.len / 2u, owned, input.len);
    return NB_OK;
}

static bool url_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

nb_status nb_url_encode(nb_str input, bool space_as_plus,
                        nb_buf *out, nb_err *err)
{
    if (!out || (input.len != 0 && !input.ptr)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid URL arguments");
        return NB_ERR_INVAL;
    }
    size_t count = 0;
    for (size_t i = 0; i < input.len; i++) {
        unsigned char c = (unsigned char)input.ptr[i];
        size_t width = url_unreserved(c) || (c == ' ' && space_as_plus) ? 1u : 3u;
        if (count > SIZE_MAX - width) {
            nb_err_set(err, NB_ERR_RANGE, "URL output is too large");
            return NB_ERR_RANGE;
        }
        count += width;
    }
    char *owned;
    char *dst;
    nb_status status = encoding_begin(out, input, count, &owned, &dst, err);
    if (status != NB_OK) {
        return status;
    }
    const char *src = owned ? owned : input.ptr;
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; i < input.len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (url_unreserved(c)) {
            dst[j++] = (char)c;
        } else if (c == ' ' && space_as_plus) {
            dst[j++] = '+';
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0x0fu];
        }
    }
    encoding_end(out, count, owned, input.len);
    return NB_OK;
}

nb_status nb_url_decode(nb_str input, bool plus_as_space,
                        nb_buf *out, nb_err *err)
{
    if (!out || (input.len != 0 && !input.ptr)) {
        nb_err_set(err, NB_ERR_INVAL, "invalid URL arguments");
        return NB_ERR_INVAL;
    }
    size_t count = 0;
    for (size_t i = 0; i < input.len; i++) {
        if (input.ptr[i] == '%') {
            if (input.len - i < 3u ||
                hex_value((unsigned char)input.ptr[i + 1u]) < 0 ||
                hex_value((unsigned char)input.ptr[i + 2u]) < 0) {
                nb_err_set(err, NB_ERR_PARSE, "invalid or truncated URL escape");
                return NB_ERR_PARSE;
            }
            i += 2u;
        }
        count++;
    }
    char *owned;
    char *dst;
    nb_status status = encoding_begin(out, input, count, &owned, &dst, err);
    if (status != NB_OK) {
        return status;
    }
    const char *src = owned ? owned : input.ptr;
    size_t j = 0;
    for (size_t i = 0; i < input.len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            int hi = hex_value((unsigned char)src[i + 1u]);
            int lo = hex_value((unsigned char)src[i + 2u]);
            dst[j++] = (char)((hi << 4) | lo);
            i += 2u;
        } else {
            dst[j++] = c == '+' && plus_as_space ? ' ' : (char)c;
        }
    }
    encoding_end(out, count, owned, input.len);
    return NB_OK;
}
