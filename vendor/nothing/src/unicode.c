#define _GNU_SOURCE
#include "nb/utf8.h"
#include "utf8proc.h"
#include <stdlib.h>
#include <string.h>

static utf8proc_ssize_t nb_utf8_iterate_bytes(const char *s, size_t len, utf8proc_int32_t *cp)
{
    if (len > 4)
        len = 4;
    return utf8proc_iterate((const utf8proc_uint8_t *)s, (utf8proc_ssize_t)len, cp);
}

const char *nb_utf8_next_cstr(const char *s, int32_t *cp)
{
    if (!s || !cp)
        return s;
    utf8proc_ssize_t count = utf8proc_iterate((const utf8proc_uint8_t *)s, -1, cp);
    if (count < 1) {
        *cp = 0xfffd;
        return s + 1;
    }
    return s + count;
}

int nb_utf8_is_continuation_byte(unsigned char byte)
{
    return byte >= 0x80u && byte < 0xc0u && utf8proc_utf8class[byte] == 0;
}

enum nb_utf8_class nb_utf8_classify_codepoint(unsigned cp)
{
    if (cp > INT32_MAX)
        return NB_UTF8_CLASS_OTHER;
    switch (utf8proc_category((utf8proc_int32_t)cp)) {
    case UTF8PROC_CATEGORY_LU:
    case UTF8PROC_CATEGORY_LT:
        return NB_UTF8_CLASS_UPPER;
    case UTF8PROC_CATEGORY_LL:
        return NB_UTF8_CLASS_LOWER;
    case UTF8PROC_CATEGORY_LM:
    case UTF8PROC_CATEGORY_LO:
        return NB_UTF8_CLASS_LETTER;
    case UTF8PROC_CATEGORY_MN:
    case UTF8PROC_CATEGORY_MC:
    case UTF8PROC_CATEGORY_ME:
        return NB_UTF8_CLASS_MARK;
    case UTF8PROC_CATEGORY_ND:
    case UTF8PROC_CATEGORY_NL:
    case UTF8PROC_CATEGORY_NO:
        return NB_UTF8_CLASS_NUMBER;
    case UTF8PROC_CATEGORY_ZS:
    case UTF8PROC_CATEGORY_ZL:
    case UTF8PROC_CATEGORY_ZP:
        return NB_UTF8_CLASS_SEPARATOR;
    default:
        return NB_UTF8_CLASS_OTHER;
    }
}

int nb_utf8_is_emoji_component_cp(unsigned cp)
{
    if (cp > INT32_MAX)
        return 0;
    const utf8proc_property_t *property = utf8proc_get_property((utf8proc_int32_t)cp);
    return property->boundclass == UTF8PROC_BOUNDCLASS_EXTENDED_PICTOGRAPHIC ||
           property->boundclass == UTF8PROC_BOUNDCLASS_REGIONAL_INDICATOR ||
           property->boundclass == UTF8PROC_BOUNDCLASS_ZWJ ||
           (property->boundclass == UTF8PROC_BOUNDCLASS_EXTEND &&
            (property->ignorable || property->charwidth == 2));
}

size_t nb_utf8_clamp_bytes(const char *s, size_t max_bytes)
{
    size_t pos = 0;

    if (!s)
        return 0;
    size_t len = strnlen(s, max_bytes);
    while (pos < len) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t count = nb_utf8_iterate_bytes(s + pos, len - pos, &cp);

        if (count < 1)
            break;
        pos += (size_t)count;
    }
    return pos;
}

size_t nb_utf8_next_codepoint_len(const char *s, size_t avail)
{
    utf8proc_int32_t cp;

    if (!s || !avail || !*s)
        return 0;
    utf8proc_ssize_t count = nb_utf8_iterate_bytes(s, avail, &cp);
    return count > 0 ? (size_t)count : 1;
}

int nb_utf8_read(const char *s, size_t avail, unsigned *cp, size_t *cp_len)
{
    utf8proc_int32_t value;

    if (!s || !avail || !*s || !cp || !cp_len)
        return 0;
    utf8proc_ssize_t count = nb_utf8_iterate_bytes(s, avail, &value);
    if (count < 1)
        return 0;
    *cp = (unsigned)value;
    *cp_len = (size_t)count;
    return 1;
}

char *nb_utf8_ellipsize_bytes(const char *src, size_t max_bytes)
{
    if (!src)
        return NULL;
    size_t len = strlen(src);
    if (len <= max_bytes) {
        return strdup(src);
    }
    static const char marker[] = "\xE2\x80\xA6(truncated)";
    const size_t marker_len = sizeof(marker) - 1;
    if (max_bytes <= marker_len) {
        size_t cut = nb_utf8_clamp_bytes(src, max_bytes);
        char *dup = malloc(cut + 1);
        if (!dup)
            return NULL;
        memcpy(dup, src, cut);
        dup[cut] = '\0';
        return dup;
    }
    size_t cut = nb_utf8_clamp_bytes(src, max_bytes - marker_len);
    char *dup = malloc(cut + marker_len + 1);
    if (!dup)
        return NULL;
    memcpy(dup, src, cut);
    memcpy(dup + cut, marker, marker_len);
    dup[cut + marker_len] = '\0';
    return dup;
}

size_t nb_utf8_sanitize_into(char *dst, const char *src, size_t src_len)
{
    size_t written = 0;

    if (!dst || !src)
        return 0;
    for (size_t pos = 0; pos < src_len;) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t count = nb_utf8_iterate_bytes(src + pos, src_len - pos, &cp);

        if (count < 1 || cp == 0) {
            pos++;
            continue;
        }
        memmove(dst + written, src + pos, (size_t)count);
        written += (size_t)count;
        pos += (size_t)count;
    }
    return written;
}

void nb_utf8_sanitize_inplace(char *s)
{
    if (!s)
        return;
    size_t clean = nb_utf8_sanitize_into(s, s, strlen(s));
    s[clean] = '\0';
}

size_t nb_utf8_copy_sanitized_clamped(char *dst, size_t dst_cap, const char *src, size_t max_bytes)
{
    size_t written = 0;

    if (!dst || !dst_cap)
        return 0;
    if (!src) {
        dst[0] = '\0';
        return 0;
    }
    size_t len = strnlen(src, max_bytes);
    for (size_t pos = 0; pos < len;) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t count = nb_utf8_iterate_bytes(src + pos, len - pos, &cp);

        if (count < 1) {
            pos++;
            continue;
        }
        if ((size_t)count >= dst_cap - written)
            break;
        memmove(dst + written, src + pos, (size_t)count);
        written += (size_t)count;
        pos += (size_t)count;
    }
    dst[written] = '\0';
    return written;
}

size_t nb_utf8_width_cstr(const char *s)
{
    if (!s)
        return 0;
    size_t n = 0;
    const char *p = s;
    while (*p) {
        int32_t cp;
        p = nb_utf8_next_cstr(p, &cp);
        n += (size_t)nb_utf8_width_cp((unsigned)cp);
    }
    return n;
}

const char *nb_utf8_advance(const char *s, size_t chars)
{
    if (!s)
        return NULL;
    while (*s && chars > 0) {
        int32_t cp;
        s = nb_utf8_next_cstr(s, &cp);
        chars--;
    }
    return s;
}

const char *nb_utf8_prev(const char *start, const char *p)
{
    if (!start || !p || p <= start)
        return start;
    size_t available = (size_t)(p - start);
    if (available > 4)
        available = 4;
    for (size_t back = 1; back <= available; back++) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t count = nb_utf8_iterate_bytes(p - back, back, &cp);

        if (count == (utf8proc_ssize_t)back)
            return p - back;
    }
    return p - 1;
}

int nb_utf8_is_unicode_space_cp(unsigned cp)
{
    return cp >= 0x80 && nb_utf8_classify_codepoint(cp) == NB_UTF8_CLASS_SEPARATOR;
}

nb_status nb_utf8_casefold(nb_str text, nb_buf *out, nb_err *err)
{
    if (!out || (!text.ptr && text.len) || text.len > PTRDIFF_MAX) {
        nb_err_set(err, NB_ERR_INVAL, "invalid Unicode case-fold input");
        return NB_ERR_INVAL;
    }
    utf8proc_uint8_t *mapped = NULL;
    utf8proc_ssize_t length = utf8proc_map((const utf8proc_uint8_t *)(text.ptr ? text.ptr : ""),
                                           (utf8proc_ssize_t)text.len, &mapped,
                                           UTF8PROC_STABLE | UTF8PROC_CASEFOLD | UTF8PROC_COMPOSE);
    if (length < 0) {
        free(mapped);
        nb_status status = length == UTF8PROC_ERROR_NOMEM ? NB_ERR_NOMEM : NB_ERR_INVAL;
        nb_err_set(err, status, "Unicode case folding failed");
        return status;
    }
    bool appended = nb_buf_append(out, mapped, (size_t)length);
    free(mapped);
    if (!appended) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory appending case-folded text");
        return NB_ERR_NOMEM;
    }
    return NB_OK;
}
