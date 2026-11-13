#ifndef NB_SIGNAL_H
#define NB_SIGNAL_H

#include "nb/err.h"
#include <signal.h>
#include "nb/thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_signal_listener nb_signal_listener;
typedef void (*nb_signal_fn)(int signal_number, void *user_data);

/* The owner thread starts the listener before creating application workers,
 * which inherit its blocked signal mask. It must stop the listener after all
 * workers exit. The signal set is exclusively owned until stop; callbacks run
 * on the listener's worker, never in an asynchronous signal handler.
 *
 * Start preserves *out on failure. Stop joins the worker, restores signal
 * dispositions and the owner's original mask, then clears *owner. A restore
 * error is reported even though the joined listener has been released.
 * Native signal numbers come from the platform's <signal.h>. */
NB_EXPORT nb_status nb_signal_listener_start(nb_signal_listener **out, const int *signals,
                                             size_t count, nb_signal_fn callback, void *user_data,
                                             nb_err *err);
NB_EXPORT nb_status nb_signal_listener_stop(nb_signal_listener **owner, nb_err *err);
/* Direct a configured signal to the listener; useful for targeted delivery
 * without signalling the entire process. */
NB_EXPORT nb_status nb_signal_listener_send(nb_signal_listener *listener, int signal_number,
                                            nb_err *err);

#ifdef __cplusplus
}
#endif

#endif
