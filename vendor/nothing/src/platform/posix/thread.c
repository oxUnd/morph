#include "nb/thread.h"

#include "nb/mem.h"
#include "platform/nb_platform.h"

#include <errno.h>
#include <time.h>


#include <pthread.h>

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

void nb_platform_log_lock(void)
{
    (void)pthread_mutex_lock(&g_log_mutex);
}

void nb_platform_log_unlock(void)
{
    (void)pthread_mutex_unlock(&g_log_mutex);
}

struct nb_mutex {
    pthread_mutex_t mutex;
};

struct nb_cond {
    pthread_cond_t cond;
    clockid_t clock;
};

struct nb_thread {
    pthread_t thread;
    bool detached;
    bool joined;
};

nb_mutex *nb_mutex_new(unsigned flags)
{
    if ((flags & ~NB_MUTEX_RECURSIVE) != 0) {
        return NULL;
    }
    nb_mutex *mutex = nb_try_alloc(sizeof(*mutex));
    if (!mutex) {
        return NULL;
    }
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        nb_free(mutex, sizeof(*mutex));
        return NULL;
    }
    int type = (flags & NB_MUTEX_RECURSIVE) != 0 ? PTHREAD_MUTEX_RECURSIVE
                                                 : PTHREAD_MUTEX_DEFAULT;
    int rc = pthread_mutexattr_settype(&attr, type);
    if (rc == 0) {
        rc = pthread_mutex_init(&mutex->mutex, &attr);
    }
    pthread_mutexattr_destroy(&attr);
    if (rc != 0) {
        nb_free(mutex, sizeof(*mutex));
        return NULL;
    }
    return mutex;
}

void nb_mutex_free(nb_mutex *mutex)
{
    if (!mutex) {
        return;
    }
    pthread_mutex_destroy(&mutex->mutex);
    nb_free(mutex, sizeof(*mutex));
}

nb_status nb_mutex_lock(nb_mutex *mutex)
{
    if (!mutex) {
        return NB_ERR_INVAL;
    }
    int rc = pthread_mutex_lock(&mutex->mutex);
    return rc == 0 ? NB_OK : nb_status_from_errno(rc, NULL);
}

nb_status nb_mutex_trylock(nb_mutex *mutex)
{
    if (!mutex) {
        return NB_ERR_INVAL;
    }
    int rc = pthread_mutex_trylock(&mutex->mutex);
    return rc == 0 ? NB_OK : nb_status_from_errno(rc, NULL);
}

void nb_mutex_unlock(nb_mutex *mutex)
{
    if (mutex) {
        pthread_mutex_unlock(&mutex->mutex);
    }
}

nb_cond *nb_cond_new(void)
{
    nb_cond *cond = nb_try_alloc(sizeof(*cond));
    if (!cond) {
        return NULL;
    }
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        nb_free(cond, sizeof(*cond));
        return NULL;
    }
    clockid_t clock = CLOCK_REALTIME;
#if defined(__linux__) && defined(CLOCK_MONOTONIC)
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0) {
        clock = CLOCK_MONOTONIC;
    }
#endif
    int rc = pthread_cond_init(&cond->cond, &attr);
    pthread_condattr_destroy(&attr);
    if (rc != 0) {
        nb_free(cond, sizeof(*cond));
        return NULL;
    }
    cond->clock = clock;
    return cond;
}

void nb_cond_free(nb_cond *cond)
{
    if (!cond) {
        return;
    }
    pthread_cond_destroy(&cond->cond);
    nb_free(cond, sizeof(*cond));
}

void nb_cond_wait(nb_cond *cond, nb_mutex *mutex)
{
    if (cond && mutex) {
        pthread_cond_wait(&cond->cond, &mutex->mutex);
    }
}

nb_status nb_cond_timedwait(nb_cond *cond, nb_mutex *mutex, int64_t timeout_ns)
{
    if (!cond || !mutex || timeout_ns < 0) {
        return NB_ERR_INVAL;
    }
    struct timespec deadline;
    if (clock_gettime(cond->clock, &deadline) != 0) {
        return nb_status_from_errno(errno, NULL);
    }
    int64_t seconds = timeout_ns / INT64_C(1000000000);
    int64_t nanos = timeout_ns % INT64_C(1000000000);
    deadline.tv_sec += (time_t)seconds;
    deadline.tv_nsec += (long)nanos;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait(&cond->cond, &mutex->mutex, &deadline);
    return rc == 0 ? NB_OK : nb_status_from_errno(rc, NULL);
}

nb_status nb_cond_wait_until(nb_cond *cond, nb_mutex *mutex, int64_t deadline_ns)
{
    if (!cond || !mutex) {
        return NB_ERR_INVAL;
    }

#if defined(CLOCK_MONOTONIC)
    bool monotonic = cond->clock == CLOCK_MONOTONIC;
#else
    bool monotonic = false;
#endif
    int64_t target = deadline_ns;
    if (!monotonic) {
        /* The condition variable uses CLOCK_REALTIME. Translate the monotonic
         * deadline by the current offset between the two clocks. */
        int64_t now_mono = nb_platform_time_monotonic_ns();
        int64_t now_real = nb_platform_time_realtime_ns();
        if (deadline_ns <= now_mono) {
            target = now_real;
        } else {
            int64_t delta = deadline_ns - now_mono;
            target = delta > INT64_MAX - now_real ? INT64_MAX
                                                  : now_real + delta;
        }
    }
    if (target < 0) {
        target = 0;
    }

    struct timespec deadline;
    deadline.tv_sec = (time_t)(target / INT64_C(1000000000));
    deadline.tv_nsec = (long)(target % INT64_C(1000000000));
    int rc = pthread_cond_timedwait(&cond->cond, &mutex->mutex, &deadline);
    return rc == 0 ? NB_OK : nb_status_from_errno(rc, NULL);
}

void nb_cond_signal(nb_cond *cond)
{
    if (cond) {
        pthread_cond_signal(&cond->cond);
    }
}

void nb_cond_broadcast(nb_cond *cond)
{
    if (cond) {
        pthread_cond_broadcast(&cond->cond);
    }
}

nb_status nb_thread_create(nb_thread **out, nb_thread_fn fn, void *arg,
                           const nb_thread_options *options)
{
    bool detached = options && options->detached;
    if (!out || !fn) {
        return NB_ERR_INVAL;
    }
    nb_thread *thread = nb_try_alloc(sizeof(*thread));
    if (!thread) {
        return NB_ERR_NOMEM;
    }
    thread->detached = false;
    thread->joined = false;
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) {
        nb_free(thread, sizeof(*thread));
        return nb_status_from_errno(rc, NULL);
    }
    if (options && options->stack_size) {
        rc = pthread_attr_setstacksize(&attr, options->stack_size);
    }
    if (rc == 0 && detached) {
        rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    }
    if (rc == 0) {
        rc = pthread_create(&thread->thread, &attr, fn, arg);
    }
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        nb_free(thread, sizeof(*thread));
        return nb_status_from_errno(rc, NULL);
    }
    thread->detached = detached;
    *out = thread;
    return NB_OK;
}

nb_status nb_thread_join(nb_thread *thread, void **result)
{
    if (!thread) {
        return NB_ERR_INVAL;
    }
    if (thread->detached || thread->joined) {
        return NB_ERR_STATE;
    }
    void *value = NULL;
    int rc = pthread_join(thread->thread, &value);
    if (rc != 0) {
        return rc == ESRCH ? NB_ERR_STATE : NB_ERR_OS;
    }
    thread->joined = true;
    if (result) {
        *result = value;
    }
    return NB_OK;
}

nb_status nb_thread_detach(nb_thread *thread)
{
    if (!thread) {
        return NB_ERR_INVAL;
    }
    if (thread->joined) {
        return NB_ERR_STATE;
    }
    if (thread->detached) {
        return NB_OK;
    }
    if (pthread_detach(thread->thread) != 0) {
        return NB_ERR_OS;
    }
    thread->detached = true;
    return NB_OK;
}

void nb_thread_free(nb_thread *thread)
{
    if (!thread) {
        return;
    }
    if (!thread->detached && !thread->joined) {
        (void)pthread_detach(thread->thread);
    }
    nb_free(thread, sizeof(*thread));
}

void nb_thread_exit(void *result)
{
    pthread_exit(result);
}

nb_thread_id nb_thread_current_id(void)
{
    pthread_t current = pthread_self();
    nb_thread_id id = {0};
    _Static_assert(sizeof(current) <= sizeof(id.value), "thread ID exceeds uintptr_t");
    memcpy(&id.value, &current, sizeof(current));
    return id;
}

bool nb_thread_id_equal(nb_thread_id left, nb_thread_id right)
{
    pthread_t a;
    pthread_t b;
    memcpy(&a, &left.value, sizeof(a));
    memcpy(&b, &right.value, sizeof(b));
    return pthread_equal(a, b) != 0;
}

bool nb_thread_is_current(const nb_thread *thread)
{
    return thread && pthread_equal(thread->thread, pthread_self()) != 0;
}

nb_status nb_cond_wait_realtime_until(nb_cond *cond, nb_mutex *mutex, int64_t deadline_ns)
{
    int64_t real = nb_platform_time_realtime_ns();
    int64_t remaining = deadline_ns > real ? deadline_ns - real : 0;
    return nb_cond_timedwait(cond, mutex, remaining);
}

nb_status nb_thread_join_take(nb_thread **owner, void **result)
{
    if (!owner || !*owner) {
        return NB_ERR_INVAL;
    }
    nb_status status = nb_thread_join(*owner, result);
    if (status == NB_OK) {
        nb_thread_free(*owner);
        *owner = NULL;
    }
    return status;
}

nb_status nb_thread_detach_take(nb_thread **owner)
{
    if (!owner || !*owner) {
        return NB_ERR_INVAL;
    }
    nb_status status = nb_thread_detach(*owner);
    if (status == NB_OK) {
        nb_thread_free(*owner);
        *owner = NULL;
    }
    return status;
}
