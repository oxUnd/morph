#include "nb/str.h"

#include "nb/mem.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

static bool is_space(unsigned char c)
{
    return isspace(c) != 0;
}

/* nb_str_from_parts() can describe an invalid view. Operations that cannot
 * report an error treat it as the empty string instead of dereferencing NULL. */
static nb_str normalize_str(nb_str s)
{
    if (s.len != 0 && !s.ptr) {
        return NB_STR_NULL;
    }
    return s;
}

nb_str nb_str_from_cstr(const char *s)
{
    nb_str str;
    str.ptr = s;
    str.len = s ? strlen(s) : 0u;
    return str;
}

nb_str nb_str_from_parts(const char *ptr, size_t len)
{
    nb_str str;
    str.ptr = ptr;
    str.len = len;
    return str;
}

bool nb_str_is_empty(nb_str s)
{
    return s.len == 0;
}

bool nb_str_eq(nb_str a, nb_str b)
{
    if (a.len != b.len) {
        return false;
    }
    if (a.len == 0) {
        return true;
    }
    if (!a.ptr || !b.ptr) {
        return false;
    }
    return memcmp(a.ptr, b.ptr, a.len) == 0;
}

bool nb_str_eq_cstr(nb_str a, const char *b)
{
    return nb_str_eq(a, nb_str_from_cstr(b));
}

bool nb_str_eq_icase(nb_str a, nb_str b)
{
    if (a.len != b.len) {
        return false;
    }
    if (a.len == 0) {
        return true;
    }
    if (!a.ptr || !b.ptr) {
        return false;
    }
    for (size_t i = 0; i < a.len; i++) {
        unsigned char ca = (unsigned char)a.ptr[i];
        unsigned char cb = (unsigned char)b.ptr[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

int nb_str_cmp(nb_str a, nb_str b)
{
    a = normalize_str(a);
    b = normalize_str(b);
    size_t n = NB_MIN(a.len, b.len);
    if (n != 0) {
        int cmp = memcmp(a.ptr, b.ptr, n);
        if (cmp != 0) {
            return cmp < 0 ? -1 : 1;
        }
    }
    if (a.len == b.len) {
        return 0;
    }
    return a.len < b.len ? -1 : 1;
}

bool nb_str_starts_with(nb_str s, nb_str prefix)
{
    if (prefix.len > s.len) {
        return false;
    }
    if (prefix.len == 0) {
        return true;
    }
    if (!s.ptr || !prefix.ptr) {
        return false;
    }
    return memcmp(s.ptr, prefix.ptr, prefix.len) == 0;
}

bool nb_str_ends_with(nb_str s, nb_str suffix)
{
    if (suffix.len > s.len) {
        return false;
    }
    if (suffix.len == 0) {
        return true;
    }
    if (!s.ptr || !suffix.ptr) {
        return false;
    }
    return memcmp(s.ptr + s.len - suffix.len, suffix.ptr, suffix.len) == 0;
}

size_t nb_str_find(nb_str haystack, nb_str needle)
{
    if (needle.len == 0) {
        return 0;
    }
    if (needle.len > haystack.len || !haystack.ptr || !needle.ptr) {
        return SIZE_MAX;
    }
    size_t last = haystack.len - needle.len;
    size_t offset = 0;
    while (offset <= last) {
        const char *match = memchr(haystack.ptr + offset, needle.ptr[0],
                                   last - offset + 1u);
        if (!match) {
            return SIZE_MAX;
        }
        size_t index = (size_t)(match - haystack.ptr);
        if (memcmp(haystack.ptr + index, needle.ptr, needle.len) == 0) {
            return index;
        }
        offset = index + 1u;
    }
    return SIZE_MAX;
}

size_t nb_str_find_char(nb_str s, char c)
{
    if (s.len == 0 || !s.ptr) {
        return SIZE_MAX;
    }
    const char *match = memchr(s.ptr, c, s.len);
    return match ? (size_t)(match - s.ptr) : SIZE_MAX;
}

void nb_split_init(nb_split_iter *it, nb_str s, char delim)
{
    NB_ASSERT(it != NULL);
    if (!s.ptr) {
        s.len = 0;
    }
    it->rest = s;
    it->delim = delim;
    it->done = false;
    it->skip_empty = false;
}

void nb_split_skip_empty(nb_split_iter *it, bool enabled)
{
    if (it) {
        it->skip_empty = enabled;
    }
}

bool nb_split_next(nb_split_iter *it, nb_str *token)
{
    if (!it) {
        return false;
    }
    for (;;) {
        if (it->done) {
            return false;
        }
        const char *start = it->rest.ptr;
        size_t len = it->rest.len;
        const char *delim = len != 0 ? memchr(start, it->delim, len) : NULL;

        nb_str result;
        if (delim) {
            result = nb_str_from_parts(start, (size_t)(delim - start));
            it->rest.ptr = delim + 1;
            it->rest.len = len - result.len - 1u;
        } else {
            result = it->rest;
            it->rest = NB_STR_NULL;
            it->done = true;
        }
        if (!it->skip_empty || result.len != 0) {
            if (token) {
                *token = result;
            }
            return true;
        }
    }
}

nb_str nb_str_ltrim(nb_str s)
{
    s = normalize_str(s);
    size_t i = 0;
    while (i < s.len && s.ptr && is_space((unsigned char)s.ptr[i])) {
        i++;
    }
    s.ptr = s.ptr ? s.ptr + i : NULL;
    s.len -= i;
    return s;
}

nb_str nb_str_rtrim(nb_str s)
{
    s = normalize_str(s);
    while (s.len > 0 && s.ptr && is_space((unsigned char)s.ptr[s.len - 1u])) {
        s.len--;
    }
    return s;
}

nb_str nb_str_trim(nb_str s)
{
    return nb_str_rtrim(nb_str_ltrim(s));
}

bool nb_str_to_i64(nb_str s, int64_t *out)
{
    if (!out || (s.len != 0 && !s.ptr)) {
        return false;
    }
    size_t i = 0;
    while (i < s.len && is_space((unsigned char)s.ptr[i])) {
        i++;
    }
    bool negative = false;
    if (i < s.len && (s.ptr[i] == '+' || s.ptr[i] == '-')) {
        negative = s.ptr[i] == '-';
        i++;
    }
    size_t digits = 0;
    uint64_t value = 0;
    uint64_t limit = negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    while (i < s.len) {
        unsigned char c = (unsigned char)s.ptr[i];
        if (c < '0' || c > '9') {
            break;
        }
        unsigned digit = (unsigned)(c - '0');
        if (value > (limit - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        digits++;
        i++;
    }
    if (digits == 0) {
        return false;
    }
    while (i < s.len && is_space((unsigned char)s.ptr[i])) {
        i++;
    }
    if (i != s.len) {
        return false;
    }
    if (negative) {
        *out = value == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)value;
    } else {
        *out = (int64_t)value;
    }
    return true;
}

bool nb_str_to_u64(nb_str s, uint64_t *out)
{
    if (!out || (s.len != 0 && !s.ptr)) {
        return false;
    }
    size_t i = 0;
    while (i < s.len && is_space((unsigned char)s.ptr[i])) {
        i++;
    }
    if (i < s.len && s.ptr[i] == '+') {
        i++;
    }
    size_t digits = 0;
    uint64_t value = 0;
    while (i < s.len) {
        unsigned char c = (unsigned char)s.ptr[i];
        if (c < '0' || c > '9') {
            break;
        }
        unsigned digit = (unsigned)(c - '0');
        if (value > (UINT64_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        digits++;
        i++;
    }
    if (digits == 0) {
        return false;
    }
    while (i < s.len && is_space((unsigned char)s.ptr[i])) {
        i++;
    }
    if (i != s.len) {
        return false;
    }
    *out = value;
    return true;
}

bool nb_str_to_f64(nb_str s, double *out)
{
    if (!out || (s.len != 0 && !s.ptr)) {
        return false;
    }
    s = nb_str_trim(s);
    if (s.len == 0 || s.len >= 4096u) {
        return false;
    }
    char stack[128];
    char *tmp = stack;
    if (s.len + 1u > sizeof(stack)) {
        tmp = nb_try_alloc(s.len + 1u);
        if (!tmp) {
            return false;
        }
    }
    memcpy(tmp, s.ptr, s.len);
    tmp[s.len] = '\0';

    errno = 0;
    char *end = NULL;
    double value = strtod(tmp, &end);
    bool ok = errno != ERANGE && end != tmp && end == tmp + s.len;
    if (tmp != stack) {
        nb_free(tmp, s.len + 1u);
    }
    if (ok) {
        *out = value;
    }
    return ok;
}

uint64_t nb_hash_bytes(const void *data, size_t len)
{
    if (!data && len != 0) {
        return 0;
    }
    const unsigned char *p = (const unsigned char *)data;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t nb_str_hash(nb_str s)
{
    return nb_hash_bytes(s.ptr, s.len);
}

nb_status nb_str_copy_to(nb_str text, char *out, size_t cap)
{
    if (!out || (!text.ptr && text.len) || !cap) {
        return NB_ERR_INVAL;
    }
    if (text.len >= cap) {
        return NB_ERR_RANGE;
    }
    memmove(out, text.ptr ? text.ptr : "", text.len);
    out[text.len] = '\0';
    return NB_OK;
}

bool nb_ascii_is_alnum(unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9');
}

bool nb_str_is_alnum(nb_str text, const char *punctuation)
{
    if (!text.ptr || !text.len) {
        return false;
    }
    for (size_t i = 0; i < text.len; i++) {
        unsigned char byte = (unsigned char)text.ptr[i];
        if (nb_ascii_is_alnum(byte)) {
            continue;
        }
        if (!byte || !punctuation || !strchr(punctuation, byte)) {
            return false;
        }
    }
    return true;
}

bool nb_ascii_is_alnum_or(unsigned char byte, const char *punctuation)
{
    return nb_ascii_is_alnum(byte) || (byte && punctuation && strchr(punctuation, byte) != NULL);
}
