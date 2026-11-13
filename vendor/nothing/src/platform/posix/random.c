/* Darwin hides arc4random_buf() when a strict POSIX feature-test macro is
 * active; ask for the full Darwin namespace before the first include. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "platform/nb_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/*
 * Prefer the kernel CSPRNG entry points and fall back to /dev/urandom. On
 * Apple/BSD arc4random_buf is already backed by the kernel RNG.
 */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#define NB_HAVE_ARC4RANDOM 1
#endif

#if !defined(NB_HAVE_ARC4RANDOM) && defined(__linux__)
#if defined(__has_include)
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#define NB_HAVE_GETRANDOM 1
#endif
#else
#include <sys/random.h>
#define NB_HAVE_GETRANDOM 1
#endif
#endif

#if defined(NB_HAVE_ARC4RANDOM)

nb_status nb_platform_random_bytes(void *out, size_t len, nb_err *err)
{
    (void)err;
    if (!out && len != 0) {
        nb_err_set(err, NB_ERR_INVAL, "output buffer is required");
        return NB_ERR_INVAL;
    }
    if (len == 0) {
        return NB_OK;
    }
    arc4random_buf(out, len);
    return NB_OK;
}

#else

static nb_status urandom_fill(unsigned char *out, size_t len, nb_err *err)
{
    int fd;
    do {
        fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        int saved = errno;
        nb_err_set_os(err, NB_ERR_OS, saved, "open /dev/urandom: %s",
                      strerror(saved));
        return NB_ERR_OS;
    }

    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, out + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            int saved = errno;
            (void)close(fd);
            nb_err_set_os(err, NB_ERR_OS, saved, "read /dev/urandom: %s",
                          strerror(saved));
            return NB_ERR_OS;
        }
        if (n == 0) {
            (void)close(fd);
            nb_err_set(err, NB_ERR_OS, "unexpected EOF from /dev/urandom");
            return NB_ERR_OS;
        }
        done += (size_t)n;
    }
    (void)close(fd);
    return NB_OK;
}

nb_status nb_platform_random_bytes(void *out, size_t len, nb_err *err)
{
    if (!out && len != 0) {
        nb_err_set(err, NB_ERR_INVAL, "output buffer is required");
        return NB_ERR_INVAL;
    }
    if (len == 0) {
        return NB_OK;
    }

    unsigned char *ptr = out;
    size_t left = len;
#if defined(NB_HAVE_GETRANDOM)
    while (left > 0) {
        ssize_t n = getrandom(ptr, left, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ENOSYS) {
                break;
            }
            int saved = errno;
            nb_err_set_os(err, NB_ERR_OS, saved, "getrandom: %s",
                          strerror(saved));
            return NB_ERR_OS;
        }
        if (n == 0) {
            break;
        }
        ptr += (size_t)n;
        left -= (size_t)n;
    }
#endif
    return left == 0 ? NB_OK : urandom_fill(ptr, left, err);
}

#endif
