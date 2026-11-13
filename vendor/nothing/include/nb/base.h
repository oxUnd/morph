#ifndef NB_BASE_H
#define NB_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NB_VERSION_MAJOR 0
#define NB_VERSION_MINOR 1
#define NB_VERSION_PATCH 0
#define NB_VERSION_STRING "0.1.0"

#if defined(__GNUC__) || defined(__clang__)
#  define NB_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define NB_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  if defined(__MINGW32__) && defined(__USE_MINGW_ANSI_STDIO)
#    define NB_PRINTF(fmt_index, arg_index) \
       __attribute__((format(gnu_printf, fmt_index, arg_index)))
#  else
#    define NB_PRINTF(fmt_index, arg_index) \
       __attribute__((format(printf, fmt_index, arg_index)))
#  endif
#  define NB_NORETURN __attribute__((noreturn))
#  define NB_UNUSED   __attribute__((unused))
#  define NB_EXPORT   __attribute__((visibility("default")))
#else
#  define NB_LIKELY(x)   (x)
#  define NB_UNLIKELY(x) (x)
#  define NB_PRINTF(fmt_index, arg_index)
#  define NB_NORETURN
#  define NB_UNUSED
#  define NB_EXPORT
#endif

#if defined(_WIN32) && defined(NB_BUILDING_DLL)
#  undef NB_EXPORT
#  define NB_EXPORT __declspec(dllexport)
#elif defined(_WIN32) && defined(NB_USING_DLL)
#  undef NB_EXPORT
#  define NB_EXPORT __declspec(dllimport)
#endif

#define NB_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define NB_MIN(a, b) ((a) < (b) ? (a) : (b))
#define NB_MAX(a, b) ((a) > (b) ? (a) : (b))
#define NB_CLAMP(v, lo, hi) (NB_MIN(NB_MAX((v), (lo)), (hi)))

#define NB_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)

#define NB_ASSERT(cond)                                                    \
    do {                                                                   \
        if (NB_UNLIKELY(!(cond))) {                                        \
            nb_assert_fail(#cond, __FILE__, __LINE__);                     \
        }                                                                  \
    } while (0)

#if defined(__GNUC__) || defined(__clang__)
#  define NB_UNREACHABLE() __builtin_unreachable()
#elif defined(_MSC_VER)
#  define NB_UNREACHABLE() __assume(0)
#else
#  define NB_UNREACHABLE() ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

NB_EXPORT NB_NORETURN void nb_assert_fail(const char *expr, const char *file, int line);
NB_EXPORT const char *nb_version(void);

#ifdef __cplusplus
}
#endif

#endif /* NB_BASE_H */
