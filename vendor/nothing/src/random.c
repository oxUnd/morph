#include "nb/random.h"

#include "platform/nb_platform.h"
#include "nb/encoding.h"
#include "nb/cleanup.h"

nb_status nb_random_bytes(void *out, size_t len, nb_err *err)
{
    if (!out && len != 0) {
        nb_err_set(err, NB_ERR_INVAL, "output buffer is required");
        return NB_ERR_INVAL;
    }
    if (len == 0) {
        return NB_OK;
    }
    return nb_platform_random_bytes(out, len, err);
}

nb_status nb_random_hex(nb_buf *out, size_t bytes, nb_err *err)
{
    if (!out) {
        nb_err_set(err, NB_ERR_INVAL, "output buffer is required");
        return NB_ERR_INVAL;
    }
    nb_buf entropy = {0};
    if (!nb_buf_reserve(&entropy, bytes)) {
        nb_err_set(err, NB_ERR_NOMEM, "out of memory allocating random bytes");
        nb_buf_destroy(&entropy);
        return NB_ERR_NOMEM;
    }
    nb_status status = nb_random_bytes(entropy.ptr, bytes, err);
    if (status == NB_OK) {
        status = nb_hex_encode(entropy.ptr, bytes, false, out, err);
    }
    nb_buf_destroy(&entropy);
    return status;
}
