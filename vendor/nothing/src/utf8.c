#include "nb/utf8.h"
#include "utf8proc.h"

#include <string.h>

static bool set_status(nb_utf8_status *status, nb_utf8_status value)
{
    if (status) {
        *status = value;
    }
    return false;
}

bool nb_utf8_decode(nb_str s, size_t *index, uint32_t *cp,
                    nb_utf8_status *status)
{
    if (!index || !cp || (!s.ptr && s.len != 0)) {
        return set_status(status, NB_UTF8_INVALID);
    }
    size_t i = *index;
    if (i >= s.len) {
        return set_status(status, NB_UTF8_TRUNCATED);
    }

    const unsigned char *p = (const unsigned char *)s.ptr;
    unsigned char b0 = p[i];
    if (b0 < 0x80u) {
        *cp = b0;
        *index = i + 1u;
        if (status) {
            *status = NB_UTF8_OK;
        }
        return true;
    }

    size_t need;
    uint32_t value;
    uint32_t minimum;
    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        need = 2u;
        value = b0 & 0x1Fu;
        minimum = 0x80u;
    } else if (b0 >= 0xE0u && b0 <= 0xEFu) {
        need = 3u;
        value = b0 & 0x0Fu;
        minimum = 0x800u;
    } else if (b0 >= 0xF0u && b0 <= 0xF4u) {
        need = 4u;
        value = b0 & 0x07u;
        minimum = 0x10000u;
    } else if (b0 == 0xC0u || b0 == 0xC1u) {
        return set_status(status, NB_UTF8_OVERLONG);
    } else if (b0 >= 0x80u && b0 <= 0xBFu) {
        return set_status(status, NB_UTF8_INVALID);
    } else {
        return set_status(status, NB_UTF8_TOO_LARGE);
    }

    if (need > s.len - i) {
        return set_status(status, NB_UTF8_TRUNCATED);
    }
    for (size_t k = 1; k < need; k++) {
        unsigned char b = p[i + k];
        if ((b & 0xC0u) != 0x80u) {
            return set_status(status, NB_UTF8_INVALID);
        }
        value = (value << 6u) | (b & 0x3Fu);
    }
    if (value < minimum) {
        return set_status(status, NB_UTF8_OVERLONG);
    }
    if (value >= 0xD800u && value <= 0xDFFFu) {
        return set_status(status, NB_UTF8_SURROGATE);
    }
    if (value > 0x10FFFFu) {
        return set_status(status, NB_UTF8_TOO_LARGE);
    }

    *cp = value;
    *index = i + need;
    if (status) {
        *status = NB_UTF8_OK;
    }
    return true;
}

nb_utf8_status nb_utf8_validate(nb_str s, size_t *error_offset)
{
    if (!s.ptr && s.len != 0) {
        if (error_offset) {
            *error_offset = 0;
        }
        return NB_UTF8_INVALID;
    }
    /* Skip the initial ASCII run without reading beyond the borrowed view.
     * memcpy permits unaligned words and avoids strict-aliasing violations. */
    size_t i = 0;
    /* Tiny inputs stay scalar so short non-ASCII strings avoid probe cost. */
    if (s.len >= 32u) {
        while (s.len - i >= 32u) {
            uint64_t a, b, c, d;
            memcpy(&a, s.ptr + i, sizeof(a));
            memcpy(&b, s.ptr + i + 8u, sizeof(b));
            memcpy(&c, s.ptr + i + 16u, sizeof(c));
            memcpy(&d, s.ptr + i + 24u, sizeof(d));
            if ((a | b | c | d) & UINT64_C(0x8080808080808080)) {
                break;
            }
            i += 32u;
        }
        while (s.len - i >= 8u) {
            uint64_t word;
            memcpy(&word, s.ptr + i, sizeof(word));
            if (word & UINT64_C(0x8080808080808080)) {
                break;
            }
            i += 8u;
        }
    }
    while (i < s.len) {
        size_t next = i;
        uint32_t cp = 0;
        nb_utf8_status status = NB_UTF8_OK;
        if (!nb_utf8_decode(s, &next, &cp, &status)) {
            if (error_offset) {
                *error_offset = i;
            }
            return status;
        }
        i = next;
    }
    if (error_offset) {
        *error_offset = s.len;
    }
    return NB_UTF8_OK;
}

bool nb_utf8_valid(nb_str s)
{
    return nb_utf8_validate(s, NULL) == NB_UTF8_OK;
}

size_t nb_utf8_count(nb_str s)
{
    size_t i = 0;
    size_t count = 0;
    while (i < s.len) {
        size_t next = i;
        uint32_t cp = 0;
        nb_utf8_status status = NB_UTF8_OK;
        if (!nb_utf8_decode(s, &next, &cp, &status)) {
            return SIZE_MAX;
        }
        i = next;
        count++;
    }
    return count;
}

size_t nb_utf8_offset(nb_str s, size_t codepoint_index)
{
    if (!s.ptr && s.len != 0) {
        return SIZE_MAX;
    }
    size_t i = 0;
    size_t count = 0;
    size_t result = SIZE_MAX;
    while (i < s.len) {
        if (count == codepoint_index) {
            result = i;
        }
        size_t next = i;
        uint32_t cp = 0;
        nb_utf8_status status = NB_UTF8_OK;
        if (!nb_utf8_decode(s, &next, &cp, &status)) {
            return SIZE_MAX;
        }
        i = next;
        count++;
    }
    if (count == codepoint_index) {
        result = i;
    }
    return result;
}

size_t nb_utf8_encode(uint32_t cp, char out[4])
{
    if (!out || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return 0;
    }
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

nb_str nb_utf8_slice(nb_str s, size_t start_cp, size_t count_cp)
{
    if (!s.ptr && s.len != 0) {
        return NB_STR_NULL;
    }
    size_t byte = 0;
    size_t cp_index = 0;
    while (cp_index < start_cp && byte < s.len) {
        size_t next = byte;
        uint32_t cp = 0;
        nb_utf8_status status = NB_UTF8_OK;
        if (!nb_utf8_decode(s, &next, &cp, &status)) {
            return NB_STR_NULL;
        }
        byte = next;
        cp_index++;
    }
    if (cp_index < start_cp) {
        return NB_STR_NULL;
    }

    size_t begin = byte;
    size_t taken = 0;
    while (taken < count_cp && byte < s.len) {
        size_t next = byte;
        uint32_t cp = 0;
        nb_utf8_status status = NB_UTF8_OK;
        if (!nb_utf8_decode(s, &next, &cp, &status)) {
            return NB_STR_NULL;
        }
        byte = next;
        taken++;
    }
    size_t end = byte;
    while (byte < s.len) {
        size_t next = byte;
        uint32_t cp = 0;
        nb_utf8_status status = NB_UTF8_OK;
        if (!nb_utf8_decode(s, &next, &cp, &status)) {
            return NB_STR_NULL;
        }
        byte = next;
    }
    return nb_str_from_parts(s.ptr ? s.ptr + begin : NULL, end - begin);
}

bool nb_utf8_is_space(uint32_t cp)
{
    return (cp >= 0x0009u && cp <= 0x000Du) ||
           cp == 0x0020u ||
           cp == 0x0085u ||
           cp == 0x00A0u ||
           cp == 0x1680u ||
           (cp >= 0x2000u && cp <= 0x200Au) ||
           cp == 0x2028u ||
           cp == 0x2029u ||
           cp == 0x202Fu ||
           cp == 0x205Fu ||
           cp == 0x3000u;
}

nb_str nb_utf8_ltrim(nb_str s)
{
    size_t i = 0;
    while (i < s.len) {
        size_t next = i;
        uint32_t cp = 0;
        nb_utf8_status status = NB_UTF8_OK;
        if (!nb_utf8_decode(s, &next, &cp, &status)) {
            break;
        }
        if (!nb_utf8_is_space(cp)) {
            break;
        }
        i = next;
    }
    return nb_str_from_parts(s.ptr ? s.ptr + i : NULL, s.len - i);
}

nb_str nb_utf8_rtrim(nb_str s)
{
    size_t end = s.len;
    while (end > 0 && s.ptr) {
        size_t start = end - 1u;
        while (start > 0 &&
               ((unsigned char)s.ptr[start] & 0xC0u) == 0x80u) {
            start--;
        }
        size_t next = 0;
        uint32_t cp = 0;
        nb_utf8_status status = NB_UTF8_OK;
        nb_str tail = nb_str_from_parts(s.ptr + start, end - start);
        if (!nb_utf8_decode(tail, &next, &cp, &status)) {
            break;
        }
        if (next != tail.len || !nb_utf8_is_space(cp)) {
            break;
        }
        end = start;
    }
    return nb_str_from_parts(s.ptr, end);
}

nb_str nb_utf8_trim(nb_str s)
{
    return nb_utf8_rtrim(nb_utf8_ltrim(s));
}

nb_str nb_utf8_strip_bom(nb_str s)
{
    if (s.ptr && s.len >= NB_UTF8_BOM_LEN &&
        memcmp(s.ptr, NB_UTF8_BOM, NB_UTF8_BOM_LEN) == 0) {
        s.ptr += NB_UTF8_BOM_LEN;
        s.len -= NB_UTF8_BOM_LEN;
    }
    return s;
}

int nb_utf8_width_cp(uint32_t cp)
{
    if (cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) {
        return -1;
    }
    return utf8proc_charwidth((utf8proc_int32_t)cp);
}

size_t nb_utf8_width(nb_str s)
{
    size_t index = 0;
    size_t columns = 0;
    while (index < s.len) {
        int width = 0;
        if (!nb_utf8_width_at(s, &index, &width)) {
            break;
        }
        columns += (size_t)width;
    }
    return columns;
}

bool nb_utf8_width_at(nb_str s, size_t *index, int *width)
{
    if (!index || !width) {
        return false;
    }
    size_t next = *index;
    uint32_t cp = 0;
    if (!nb_utf8_decode(s, &next, &cp, NULL)) {
        return false;
    }
    int columns = nb_utf8_width_cp(cp);
    if (columns < 0) {
        return false;
    }
    *index = next;
    *width = columns;
    return true;
}
