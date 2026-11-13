#ifndef NB_POLL_H
#define NB_POLL_H

#include "nb/base.h"
#include "nb/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nb_poll_item {
    int fd;
    bool want_read;
    bool want_write;
    bool ready_read;
    bool ready_write;
    bool error;
} nb_poll_item;

/* Waits for fd readiness. timeout_ms < 0 waits forever; 0 polls once.
 * Returns NB_OK when at least one item is ready, NB_ERR_TIMEOUT when the
 * timeout expires first, and NB_ERR_INVAL/NB_ERR_OS on failure. ready may be
 * NULL and receives the number of ready items. POSIX accepts any pollable fd;
 * the Windows backend supports socket descriptors only. */
NB_EXPORT nb_status nb_poll(nb_poll_item *items, size_t count, int timeout_ms,
                            size_t *ready, nb_err *err);

#ifdef __cplusplus
}
#endif

#endif /* NB_POLL_H */
