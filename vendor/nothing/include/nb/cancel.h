#ifndef NB_CANCEL_H
#define NB_CANCEL_H

#include "nb/base.h"
#include "nb/thread.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A small thread-safe cancellation flag. Reset requires exclusive ownership
 * after all readers have stopped; request/requested are safe concurrently. */
typedef struct nb_cancel {
    nb_atomic_int cancelled;
} nb_cancel;

NB_EXPORT void nb_cancel_init(nb_cancel *cancel);
NB_EXPORT void nb_cancel_reset(nb_cancel *cancel);
NB_EXPORT void nb_cancel_request(nb_cancel *cancel);
NB_EXPORT bool nb_cancel_requested(const nb_cancel *cancel);

#ifdef __cplusplus
}
#endif

#endif /* NB_CANCEL_H */
