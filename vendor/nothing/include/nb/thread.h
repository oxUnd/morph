#ifndef NB_THREAD_H
#define NB_THREAD_H

#include "nb/base.h"
#include "nb/err.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Atomics use plain storage with compiler builtins so the public headers are
 * valid strict C++17 as well as C11. The C and C++ views share one ABI. */
/* NULL loads/exchanges return zero; NULL stores are ignored. This permits
 * optional cancellation flags without a separate wrapper type. */
typedef int nb_atomic_int;
typedef int64_t nb_atomic_i64;

#if defined(_MSC_VER)
static inline void nb_atomic_int_init(nb_atomic_int *value, int initial) { *value = initial; }

static inline int nb_atomic_int_load(const nb_atomic_int *value)
{
    if (!value) {
        return 0;
    }
    return (int)_InterlockedCompareExchange((volatile long *)(void *)value, 0, 0);
}

static inline void nb_atomic_int_store(nb_atomic_int *value, int desired)
{
    if (!value) {
        return;
    }
    (void)_InterlockedExchange((volatile long *)(void *)value, (long)desired);
}

static inline int nb_atomic_int_exchange(nb_atomic_int *value, int desired)
{
    return value ? (int)_InterlockedExchange((volatile long *)(void *)value, (long)desired) : 0;
}

static inline int nb_atomic_int_fetch_add(nb_atomic_int *value, int delta)
{
    return (int)_InterlockedExchangeAdd((volatile long *)(void *)value, (long)delta);
}

static inline bool nb_atomic_int_cas(nb_atomic_int *value, int *expected,
                                     int desired)
{
    long previous =
        _InterlockedCompareExchange((volatile long *)(void *)value, (long)desired, (long)*expected);
    bool matched = previous == (long)*expected;
    if (!matched) {
        *expected = (int)previous;
    }
    return matched;
}

static inline void nb_atomic_i64_init(nb_atomic_i64 *value, int64_t initial) { *value = initial; }

static inline int64_t nb_atomic_i64_load(const nb_atomic_i64 *value)
{
    return (int64_t)_InterlockedCompareExchange64((volatile __int64 *)(void *)value, 0, 0);
}

static inline void nb_atomic_i64_store(nb_atomic_i64 *value, int64_t desired)
{
    (void)_InterlockedExchange64((volatile __int64 *)(void *)value, (__int64)desired);
}

static inline int64_t nb_atomic_i64_fetch_add(nb_atomic_i64 *value,
                                               int64_t delta)
{
    return (int64_t)_InterlockedExchangeAdd64((volatile __int64 *)(void *)value, (__int64)delta);
}
#else
static inline void nb_atomic_int_init(nb_atomic_int *value, int initial)
{
    __atomic_store_n(value, initial, __ATOMIC_RELAXED);
}

static inline int nb_atomic_int_load(const nb_atomic_int *value)
{
    if (!value) {
        return 0;
    }
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline void nb_atomic_int_store(nb_atomic_int *value, int desired)
{
    if (!value) {
        return;
    }
    __atomic_store_n(value, desired, __ATOMIC_RELEASE);
}

static inline int nb_atomic_int_exchange(nb_atomic_int *value, int desired)
{
    return value ? __atomic_exchange_n(value, desired, __ATOMIC_ACQ_REL) : 0;
}

static inline int nb_atomic_int_fetch_add(nb_atomic_int *value, int delta)
{
    return __atomic_fetch_add(value, delta, __ATOMIC_ACQ_REL);
}

static inline bool nb_atomic_int_cas(nb_atomic_int *value, int *expected, int desired)
{
    int expected_value = *expected;
    bool matched = __atomic_compare_exchange_n(value, &expected_value, desired, false,
                                               __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    if (!matched) {
        *expected = expected_value;
    }
    return matched;
}

static inline void nb_atomic_i64_init(nb_atomic_i64 *value, int64_t initial)
{
    __atomic_store_n(value, initial, __ATOMIC_RELAXED);
}

static inline int64_t nb_atomic_i64_load(const nb_atomic_i64 *value)
{
    if (!value) {
        return 0;
    }
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static inline void nb_atomic_i64_store(nb_atomic_i64 *value, int64_t desired)
{
    if (!value) {
        return;
    }
    __atomic_store_n(value, desired, __ATOMIC_RELEASE);
}

static inline int64_t nb_atomic_i64_fetch_add(nb_atomic_i64 *value, int64_t delta)
{
    return __atomic_fetch_add(value, delta, __ATOMIC_ACQ_REL);
}
#endif

typedef struct nb_mutex nb_mutex;
typedef struct nb_cond nb_cond;
typedef struct nb_thread nb_thread;
typedef void *(*nb_thread_fn)(void *arg);

#define NB_MUTEX_RECURSIVE (1u << 0)

/* flags is 0 or NB_MUTEX_RECURSIVE. Windows critical sections are already
 * recursive; POSIX uses PTHREAD_MUTEX_RECURSIVE when requested. */
NB_EXPORT nb_mutex *nb_mutex_new(unsigned flags);
NB_EXPORT void nb_mutex_free(nb_mutex *mutex);
NB_EXPORT nb_status nb_mutex_lock(nb_mutex *mutex);
NB_EXPORT nb_status nb_mutex_trylock(nb_mutex *mutex);
NB_EXPORT void nb_mutex_unlock(nb_mutex *mutex);

NB_EXPORT nb_cond *nb_cond_new(void);
NB_EXPORT void nb_cond_free(nb_cond *cond);
NB_EXPORT void nb_cond_wait(nb_cond *cond, nb_mutex *mutex);

/* Waits for at most timeout_ns nanoseconds. Returns NB_OK when signalled,
 * NB_ERR_TIMEOUT on timeout, or another status on error. */
NB_EXPORT nb_status nb_cond_timedwait(nb_cond *cond, nb_mutex *mutex, int64_t timeout_ns);
/* deadline_ns is on the nb_time_monotonic_ns() timeline. */
NB_EXPORT nb_status nb_cond_wait_until(nb_cond *cond, nb_mutex *mutex, int64_t deadline_ns);
/* Absolute wall-clock deadline, in nanoseconds since the Unix epoch. */
NB_EXPORT nb_status nb_cond_wait_realtime_until(nb_cond *cond, nb_mutex *mutex,
                                                int64_t deadline_ns);
NB_EXPORT void nb_cond_signal(nb_cond *cond);
NB_EXPORT void nb_cond_broadcast(nb_cond *cond);

typedef struct nb_thread_options {
    bool detached;
    size_t stack_size;
} nb_thread_options;

typedef struct nb_thread_id {
    uintptr_t value;
} nb_thread_id;

NB_EXPORT nb_thread_id nb_thread_current_id(void);
NB_EXPORT bool nb_thread_id_equal(nb_thread_id left, nb_thread_id right);
NB_EXPORT bool nb_thread_is_current(const nb_thread *thread);

/* options may be NULL for a joinable thread with the platform default stack. */
NB_EXPORT nb_status nb_thread_create(nb_thread **out, nb_thread_fn fn, void *arg,
                                     const nb_thread_options *options);
NB_EXPORT nb_status nb_thread_join(nb_thread *thread, void **result);
NB_EXPORT nb_status nb_thread_detach(nb_thread *thread);
/* Success joins/detaches, frees the handle, and clears *owner. Failure keeps
 * the owner unchanged. */
NB_EXPORT nb_status nb_thread_join_take(nb_thread **owner, void **result);
NB_EXPORT nb_status nb_thread_detach_take(nb_thread **owner);
NB_EXPORT void nb_thread_free(nb_thread *thread);
NB_EXPORT NB_NORETURN void nb_thread_exit(void *result);

#ifdef __cplusplus
}
#endif

#endif /* NB_THREAD_H */
