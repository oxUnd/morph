#include "platform/nb_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <limits.h>

nb_status nb_platform_random_bytes(void *out, size_t len, nb_err *err)
{
    if (!out && len != 0) {
        nb_err_set(err, NB_ERR_INVAL, "output buffer is required");
        return NB_ERR_INVAL;
    }

    unsigned char *ptr = out;
    size_t left = len;
    while (left > 0) {
        ULONG chunk = left > (size_t)ULONG_MAX ? ULONG_MAX : (ULONG)left;
        NTSTATUS status = BCryptGenRandom(NULL, ptr, chunk,
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(status)) {
            nb_err_set_os(err, NB_ERR_OS, (int)status,
                          "BCryptGenRandom failed (NTSTATUS 0x%08lx)",
                          (unsigned long)status);
            return NB_ERR_OS;
        }
        ptr += chunk;
        left -= chunk;
    }
    return NB_OK;
}
