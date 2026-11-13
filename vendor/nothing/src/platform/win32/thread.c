#include "nb/thread.h"

#include "nb/mem.h"
#include "platform/nb_platform.h"

/* CONDITION_VARIABLE and SleepConditionVariableCS need Windows Vista or
 * newer. CMake defines this for the target; keep standalone builds working. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <process.h>

/*
 * Windows backend: CRITICAL_SECTION mutexes, CONDITION_VARIABLE condition
 * variables and _beginthreadex threads.
 *
 * Each running thread owns an nb_thread_ctx holding fn/arg/result plus an
 * interlocked state. A joinable context is freed by nb_thread_join(); a
 * detached context is freed by the worker itself when it finishes. Detach may
 * race with the worker returning, so ownership is decided by one
 * InterlockedCompareExchange/InterlockedExchange pair instead of a plain flag.
 */

enum {
    NB_THREAD_CTX_JOINABLE = 0,
    NB_THREAD_CTX_DETACHED = 1,
    NB_THREAD_CTX_FINISHED = 2
};

typedef struct nb_thread_ctx {
    nb_thread_fn fn;
    void *arg;
    void *result;
    volatile LONG state;
} nb_thread_ctx;

struct nb_mutex {
    CRITICAL_SECTION cs;
};

struct nb_cond {
    CONDITION_VARIABLE cv;
};

struct nb_thread {
    HANDLE handle;
    nb_thread_ctx *ctx;
    bool detached;
    bool joined;
};

/* Context of the calling worker. nb_thread_exit uses it to publish its result
 * and to honour detached ownership before terminating the thread. */
static _Thread_local nb_thread_ctx *nb_thread_current_ctx;

static SRWLOCK g_log_lock = SRWLOCK_INIT;

void nb_platform_log_lock(void)
{
    AcquireSRWLockExclusive(&g_log_lock);
}

void nb_platform_log_unlock(void)
{
    ReleaseSRWLockExclusive(&g_log_lock);
}

static void nb_thread_ctx_finish(nb_thread_ctx *ctx, void *result)
{
    ctx->result = result;
    if (InterlockedCompareExchange(&ctx->state, NB_THREAD_CTX_FINISHED,
                                   NB_THREAD_CTX_JOINABLE) ==
        NB_THREAD_CTX_DETACHED) {
        nb_free(ctx, sizeof(*ctx));
    }
}

static unsigned __stdcall nb_thread_proc(void *param)
{
    nb_thread_ctx *ctx = param;
    nb_thread_current_ctx = ctx;
    void *result = ctx->fn(ctx->arg);
    nb_thread_ctx_finish(ctx, result);
    return 0;
}

nb_mutex *nb_mutex_new(unsigned flags)
{
    /* CRITICAL_SECTION is always recursive; the flag needs no translation. */
    (void)flags;
    nb_mutex *mutex = nb_try_alloc(sizeof(*mutex));
    if (!mutex) {
        return NULL;
    }
    InitializeCriticalSection(&mutex->cs);
    return mutex;
}

void nb_mutex_free(nb_mutex *mutex)
{
    if (!mutex) {
        return;
    }
    DeleteCriticalSection(&mutex->cs);
    nb_free(mutex, sizeof(*mutex));
}

nb_status nb_mutex_lock(nb_mutex *mutex)
{
    if (!mutex) {
        return NB_ERR_INVAL;
    }
    EnterCriticalSection(&mutex->cs);
    return NB_OK;
}

nb_status nb_mutex_trylock(nb_mutex *mutex)
{
    if (!mutex) {
        return NB_ERR_INVAL;
    }
    return TryEnterCriticalSection(&mutex->cs) ? NB_OK : NB_ERR_BUSY;
}

void nb_mutex_unlock(nb_mutex *mutex)
{
    if (mutex) {
        LeaveCriticalSection(&mutex->cs);
    }
}

nb_cond *nb_cond_new(void)
{
    nb_cond *cond = nb_try_alloc(sizeof(*cond));
    if (!cond) {
        return NULL;
    }
    InitializeConditionVariable(&cond->cv);
    return cond;
}

void nb_cond_free(nb_cond *cond)
{
    if (!cond) {
        return;
    }
    /* CONDITION_VARIABLE has no destroy/cleanup call. */
    nb_free(cond, sizeof(*cond));
}

void nb_cond_wait(nb_cond *cond, nb_mutex *mutex)
{
    if (cond && mutex) {
        (void)SleepConditionVariableCS(&cond->cv, &mutex->cs, INFINITE);
    }
}

nb_status nb_cond_timedwait(nb_cond *cond, nb_mutex *mutex, int64_t timeout_ns)
{
    if (!cond || !mutex || timeout_ns < 0) {
        return NB_ERR_INVAL;
    }

    /* Round up to whole milliseconds. A zero timeout would mean INFINITE to
     * SleepConditionVariableCS, so a sub-millisecond wait becomes 1 ms. */
    uint64_t ms = ((uint64_t)timeout_ns + UINT64_C(999999)) /
                  UINT64_C(1000000);
    if (ms == 0) {
        ms = 1;
    }
    /* INFINITE would wait forever; clamp huge timeouts to the largest finite
     * DWORD instead. */
    if (ms >= (uint64_t)INFINITE) {
        ms = (uint64_t)INFINITE - UINT64_C(1);
    }

    BOOL signalled = SleepConditionVariableCS(&cond->cv, &mutex->cs,
                                              (DWORD)ms);
    return signalled ? NB_OK : GetLastError() == ERROR_TIMEOUT ? NB_ERR_TIMEOUT : NB_ERR_OS;
}

nb_status nb_cond_wait_until(nb_cond *cond, nb_mutex *mutex, int64_t deadline_ns)
{
    if (!cond || !mutex) {
        return NB_ERR_INVAL;
    }

    int64_t now = nb_platform_time_monotonic_ns();
    DWORD ms = 0;
    if (deadline_ns > now) {
        /* Convert the remaining interval without overflowing int64 and round
         * up so a sub-millisecond remainder still waits briefly. */
        uint64_t remaining = (uint64_t)(deadline_ns - now);
        uint64_t millis = remaining / UINT64_C(1000000);
        if (remaining % UINT64_C(1000000) != 0) {
            millis++;
        }
        /* INFINITE would block forever; clamp to the largest finite wait. */
        if (millis >= (uint64_t)INFINITE) {
            millis = (uint64_t)INFINITE - UINT64_C(1);
        }
        ms = (DWORD)millis;
    }

    BOOL signalled = SleepConditionVariableCS(&cond->cv, &mutex->cs, ms);
    return signalled ? NB_OK : GetLastError() == ERROR_TIMEOUT ? NB_ERR_TIMEOUT : NB_ERR_OS;
}

void nb_cond_signal(nb_cond *cond)
{
    if (cond) {
        WakeConditionVariable(&cond->cv);
    }
}

void nb_cond_broadcast(nb_cond *cond)
{
    if (cond) {
        WakeAllConditionVariable(&cond->cv);
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
    nb_thread_ctx *ctx = nb_try_alloc(sizeof(*ctx));
    if (!ctx) {
        nb_free(thread, sizeof(*thread));
        return NB_ERR_NOMEM;
    }

    ctx->fn = fn;
    ctx->arg = arg;
    ctx->result = NULL;
    InterlockedExchange(&ctx->state, detached ? NB_THREAD_CTX_DETACHED
                                              : NB_THREAD_CTX_JOINABLE);

    thread->handle = NULL;
    thread->ctx = ctx;
    thread->detached = false;
    thread->joined = false;

    uintptr_t raw = _beginthreadex(NULL, options ? (unsigned)options->stack_size : 0u,
                                   nb_thread_proc, ctx, 0, NULL);
    if (!raw) {
        nb_free(ctx, sizeof(*ctx));
        nb_free(thread, sizeof(*thread));
        return NB_ERR_OS;
    }
    thread->handle = (HANDLE)raw;

    if (detached) {
        /* The worker frees ctx when it finishes; we only keep the wrapper. */
        thread->ctx = NULL;
        CloseHandle(thread->handle);
        thread->handle = NULL;
        thread->detached = true;
    }

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

    DWORD rc = WaitForSingleObject(thread->handle, INFINITE);
    if (rc != WAIT_OBJECT_0) {
        return NB_ERR_OS;
    }

    void *value = thread->ctx ? thread->ctx->result : NULL;
    CloseHandle(thread->handle);
    thread->handle = NULL;
    nb_free(thread->ctx, sizeof(*thread->ctx));
    thread->ctx = NULL;
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

    if (thread->ctx) {
        if (InterlockedExchange(&thread->ctx->state,
                                NB_THREAD_CTX_DETACHED) ==
            NB_THREAD_CTX_FINISHED) {
            /* The worker already finished and left the context for us. */
            nb_free(thread->ctx, sizeof(*thread->ctx));
        }
        /* Either freed above or now owned by the running worker. */
        thread->ctx = NULL;
    }
    if (thread->handle) {
        CloseHandle(thread->handle);
        thread->handle = NULL;
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
        (void)nb_thread_detach(thread);
    }
    nb_free(thread, sizeof(*thread));
}

void nb_thread_exit(void *result)
{
    nb_thread_ctx *ctx = nb_thread_current_ctx;
    if (ctx) {
        nb_thread_current_ctx = NULL;
        nb_thread_ctx_finish(ctx, result);
    }
    _endthreadex(0);
    ExitThread(0);
}

nb_thread_id nb_thread_current_id(void)
{
    nb_thread_id id = {(uintptr_t)GetCurrentThreadId()};
    return id;
}

bool nb_thread_id_equal(nb_thread_id left, nb_thread_id right) { return left.value == right.value; }

bool nb_thread_is_current(const nb_thread *thread)
{
    return thread && GetThreadId(thread->handle) == GetCurrentThreadId();
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
