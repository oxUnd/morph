#ifndef NB_ENCODING_H
#define NB_ENCODING_H

#include "nb/base.h"
#include "nb/buf.h"
#include "nb/err.h"
#include "nb/str.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nb_b64_variant {
    NB_B64_STD = 0,
    NB_B64_URL = 1
} nb_b64_variant;

/* All buffer encoders/decoders append to out. Input may borrow initialized
 * bytes from out. Errors preserve its contents and cursor; allocation failure
 * may set out's sticky error flag. Output size overflow returns NB_ERR_RANGE. */
NB_EXPORT nb_status nb_base64_encode(const void *data, size_t len,
                                     nb_b64_variant variant, bool pad,
                                     nb_buf *out, nb_err *err);
NB_EXPORT nb_status nb_base64_decode(nb_str input, nb_b64_variant variant,
                                     nb_buf *out, nb_err *err);

/*
 * Validates a base64 payload without allocating. Space, tab, CR and LF between
 * characters are ignored. Padding is optional but, when present, must be
 * terminal, at most two characters, and make the non-space length a multiple
 * of four. Unpadded input may have any length except one modulo four. Unused
 * trailing bits must be zero, matching nb_base64_decode().
 */
NB_EXPORT bool nb_base64_valid(nb_str input, nb_b64_variant variant);

NB_EXPORT nb_status nb_hex_encode(const void *data, size_t len, bool uppercase,
                                  nb_buf *out, nb_err *err);
/* Writes a NUL-terminated hex string into out. Returns NB_ERR_RANGE when cap
 * is smaller than len * 2 + 1. */
NB_EXPORT nb_status nb_hex_encode_to(const void *data, size_t len,
                                     bool uppercase, char *out, size_t cap,
                                     nb_err *err);
NB_EXPORT nb_status nb_hex_decode(nb_str input, nb_buf *out, nb_err *err);

NB_EXPORT nb_status nb_url_encode(nb_str input, bool space_as_plus,
                                  nb_buf *out, nb_err *err);
NB_EXPORT nb_status nb_url_decode(nb_str input, bool plus_as_space,
                                  nb_buf *out, nb_err *err);

#ifdef __cplusplus
}
#endif

#endif /* NB_ENCODING_H */
