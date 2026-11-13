#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include "test.h"

#include "nb/signal.h"
#include "nb/time.h"

#if !defined(_WIN32)
#include <pthread.h>

typedef struct signal_observation {
    nb_atomic_int last;
    nb_atomic_int count;
} signal_observation;

static void record_signal(int number, void *opaque)
{
    signal_observation *observation = opaque;
    nb_atomic_int_store(&observation->last, number);
    nb_atomic_int_fetch_add(&observation->count, 1);
}

typedef struct foreign_stop {
    nb_signal_listener **listener;
    nb_status status;
} foreign_stop;

static void *stop_from_foreign_thread(void *opaque)
{
    foreign_stop *state = opaque;
    state->status = nb_signal_listener_stop(state->listener, NULL);
    return NULL;
}

static void wait_count(signal_observation *state, int count)
{
    for (int i = 0; i < 100 && nb_atomic_int_load(&state->count) < count; i++) {
        nb_time_sleep_ms(5);
    }
    NB_CHECK_INT(nb_atomic_int_load(&state->count), count);
}
#endif

void test_signal(void)
{
    nb_signal_listener *listener = NULL;
    NB_CHECK_INT(nb_signal_listener_stop(&listener, NULL), NB_OK);
    NB_CHECK_INT(nb_signal_listener_stop(NULL, NULL), NB_OK);
    NB_CHECK_INT(nb_atomic_int_load(NULL), 0);
    NB_CHECK_INT(nb_atomic_int_exchange(NULL, 42), 0);
    nb_atomic_int_store(NULL, 1);
#if defined(_WIN32)
    NB_CHECK_INT(nb_signal_listener_start(&listener, NULL, 0, NULL, NULL, NULL),
                 NB_ERR_UNSUPPORTED);
#else
    signal_observation state = {0};
    int bad[] = {SIGKILL};
    NB_CHECK_INT(nb_signal_listener_start(&listener, bad, 1, record_signal, &state, NULL),
                 NB_ERR_INVAL);
    NB_CHECK(listener == NULL);
    int duplicate[] = {SIGWINCH, SIGWINCH};
    NB_CHECK_INT(nb_signal_listener_start(&listener, duplicate, 2, record_signal, &state, NULL),
                 NB_ERR_INVAL);
    sigset_t previous;
    NB_CHECK_INT(pthread_sigmask(SIG_BLOCK, NULL, &previous), 0);
    struct sigaction previous_action;
    NB_CHECK_INT(sigaction(SIGWINCH, NULL, &previous_action), 0);
    int signals[] = {SIGWINCH, SIGUSR1};
    NB_CHECK_INT(nb_signal_listener_start(&listener, signals, 2, record_signal, &state, NULL),
                 NB_OK);
    NB_CHECK(listener != NULL);
    if (!listener) {
        return;
    }
    NB_CHECK_INT(nb_signal_listener_start(&listener, signals, 2, record_signal, &state, NULL),
                 NB_ERR_EXISTS);
    NB_CHECK_INT(nb_signal_listener_send(listener, SIGTERM, NULL), NB_ERR_INVAL);
    NB_CHECK_INT(nb_signal_listener_send(listener, SIGWINCH, NULL), NB_OK);
    wait_count(&state, 1);
    NB_CHECK_INT(nb_atomic_int_load(&state.last), SIGWINCH);
    NB_CHECK_INT(nb_signal_listener_send(listener, SIGUSR1, NULL), NB_OK);
    wait_count(&state, 2);
    NB_CHECK_INT(nb_atomic_int_load(&state.last), SIGUSR1);
    foreign_stop stop = {&listener, NB_OK};
    nb_thread *thread = NULL;
    NB_CHECK_INT(nb_thread_create(&thread, stop_from_foreign_thread, &stop, NULL), NB_OK);
    if (thread) {
        NB_CHECK_INT(nb_thread_join_take(&thread, NULL), NB_OK);
        NB_CHECK_INT(stop.status, NB_ERR_STATE);
        NB_CHECK(listener != NULL);
    }
    NB_CHECK_INT(nb_signal_listener_stop(&listener, NULL), NB_OK);
    NB_CHECK(listener == NULL);
    NB_CHECK_INT(nb_atomic_int_load(&state.count), 2);
    sigset_t after;
    NB_CHECK_INT(pthread_sigmask(SIG_BLOCK, NULL, &after), 0);
    NB_CHECK_INT(sigismember(&previous, SIGWINCH), sigismember(&after, SIGWINCH));
    NB_CHECK_INT(sigismember(&previous, SIGUSR1), sigismember(&after, SIGUSR1));
    struct sigaction after_action;
    NB_CHECK_INT(sigaction(SIGWINCH, NULL, &after_action), 0);
    NB_CHECK(previous_action.sa_handler == after_action.sa_handler);
#endif
}
