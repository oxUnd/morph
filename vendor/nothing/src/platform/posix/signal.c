#include "nb/signal.h"

#include "nb/mem.h"
#include "nb/vec.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

typedef struct signal_entry {
    int number;
    struct sigaction previous;
    bool installed;
} signal_entry;

struct nb_signal_listener {
    pthread_t thread;
    pthread_t owner;
    sigset_t signals;
    sigset_t previous_mask;
    nb_vec entries;
    nb_atomic_int stopping;
    nb_signal_fn callback;
    void *user_data;
};

/* Darwin discards default-ignored signals before sigwait can consume them.
 * This stateless disposition keeps blocked signals pending on that platform. */
static void keep_signal_pending(int number) { (void)number; }

static nb_status restore_listener(nb_signal_listener *listener, nb_err *err)
{
    nb_status status = NB_OK;
    for (size_t i = nb_vec_len(&listener->entries); i > 0; i--) {
        signal_entry *entry = nb_vec_at(&listener->entries, i - 1u);
        if (entry->installed && sigaction(entry->number, &entry->previous, NULL) != 0) {
            int error = errno;
            if (status == NB_OK) {
                status = nb_status_from_errno(error, err);
            }
        }
    }
    int error = pthread_sigmask(SIG_SETMASK, &listener->previous_mask, NULL);
    if (error && status == NB_OK) {
        status = nb_status_from_errno(error, err);
    }
    return status;
}

static void listener_release(nb_signal_listener *listener)
{
    nb_vec_destroy(&listener->entries);
    nb_free(listener, sizeof(*listener));
}

static void *listen_signals(void *opaque)
{
    nb_signal_listener *listener = opaque;
    for (;;) {
        int number = 0;
        int error = sigwait(&listener->signals, &number);
        if (nb_atomic_int_load(&listener->stopping)) {
            return NULL;
        }
        if (error != 0) {
            return NULL;
        }
        listener->callback(number, listener->user_data);
    }
}

nb_status nb_signal_listener_start(nb_signal_listener **out, const int *signals, size_t count,
                                   nb_signal_fn callback, void *user_data, nb_err *err)
{
    if (!out || !signals || count == 0 || !callback) {
        return NB_ERR_INVAL;
    }
    if (*out) {
        return NB_ERR_EXISTS;
    }
    nb_signal_listener *listener = nb_try_alloc(sizeof(*listener));
    if (!listener) {
        return NB_ERR_NOMEM;
    }
    memset(listener, 0, sizeof(*listener));
    nb_vec_init(&listener->entries, sizeof(signal_entry));
    if (!nb_vec_reserve(&listener->entries, count)) {
        listener_release(listener);
        return NB_ERR_NOMEM;
    }
    sigemptyset(&listener->signals);
    for (size_t i = 0; i < count; i++) {
        int number = signals[i];
        if (number <= 0 || number == SIGKILL || number == SIGSTOP ||
            sigismember(&listener->signals, number) == 1) {
            listener_release(listener);
            return NB_ERR_INVAL;
        }
        signal_entry entry;
        memset(&entry, 0, sizeof(entry));
        entry.number = number;
        if (sigaddset(&listener->signals, number) != 0 ||
            !nb_vec_push_copy(&listener->entries, &entry)) {
            listener_release(listener);
            return NB_ERR_INVAL;
        }
    }
    int error = pthread_sigmask(SIG_BLOCK, &listener->signals, &listener->previous_mask);
    if (error) {
        listener_release(listener);
        return nb_status_from_errno(error, err);
    }
    for (size_t i = 0; i < count; i++) {
        signal_entry *entry = nb_vec_at(&listener->entries, i);
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = keep_signal_pending;
        sigemptyset(&action.sa_mask);
        if (sigaction(entry->number, &action, &entry->previous) != 0) {
            error = errno;
            (void)restore_listener(listener, NULL);
            listener_release(listener);
            return nb_status_from_errno(error, err);
        }
        entry->installed = true;
    }
    listener->callback = callback;
    listener->user_data = user_data;
    listener->owner = pthread_self();
    nb_atomic_int_init(&listener->stopping, 0);
    error = pthread_create(&listener->thread, NULL, listen_signals, listener);
    if (error) {
        (void)restore_listener(listener, NULL);
        listener_release(listener);
        return nb_status_from_errno(error, err);
    }
    *out = listener;
    return NB_OK;
}

nb_status nb_signal_listener_send(nb_signal_listener *listener, int signal_number, nb_err *err)
{
    if (!listener || sigismember(&listener->signals, signal_number) != 1) {
        return NB_ERR_INVAL;
    }
    int error = pthread_kill(listener->thread, signal_number);
    return error == 0 ? NB_OK : nb_status_from_errno(error, err);
}

nb_status nb_signal_listener_stop(nb_signal_listener **owner, nb_err *err)
{
    if (!owner || !*owner) {
        return NB_OK;
    }
    nb_signal_listener *listener = *owner;
    if (!pthread_equal(listener->owner, pthread_self())) {
        return NB_ERR_STATE;
    }
    nb_atomic_int_store(&listener->stopping, 1);
    signal_entry *wake = nb_vec_at(&listener->entries, 0);
    int error = pthread_kill(listener->thread, wake->number);
    if (error && error != ESRCH) {
        return nb_status_from_errno(error, err);
    }
    error = pthread_join(listener->thread, NULL);
    if (error) {
        return nb_status_from_errno(error, err);
    }
    nb_status status = restore_listener(listener, err);
    listener_release(listener);
    *owner = NULL;
    return status;
}
