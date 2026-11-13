#ifndef NB_RANDOM_H
#define NB_RANDOM_H

#include "nb/base.h"
#include "nb/err.h"
#include "nb/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cryptographically secure random bytes. len == 0 is a successful no-op. */
NB_EXPORT nb_status nb_random_bytes(void *out, size_t len, nb_err *err);

/* Appends cryptographically random bytes as lowercase hexadecimal. bytes is
 * the amount of entropy, so the resulting text has twice that length.
 * Failure preserves out. */
NB_EXPORT nb_status nb_random_hex(nb_buf *out, size_t bytes, nb_err *err);

#ifdef __cplusplus
}
#endif

#endif /* NB_RANDOM_H */
