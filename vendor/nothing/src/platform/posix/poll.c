#include "platform/nb_platform.h"

#include "nb/mem.h"

#include <errno.h>
#include <poll.h>
#include <string.h>

#define NB_POLL_STACK_ITEMS 16u

/* Returns the remaining wait in milliseconds, -1 for an infinite wait and 0
 * once the deadline passed. */
static int remaining_ms(int64_t deadline_ns)
{
    if (deadline_ns < 0) {
        return -1;
    }
    int64_t now = nb_platform_time_monotonic_ns();
    if (now >= deadline_ns) {
        return 0;
    }
    int64_t ms = (deadline_ns - now) / INT64_C(1000000);
    if (ms < 1) {
        ms = 1;
    }
    if (ms > INT32_MAX) {
        ms = INT32_MAX;
    }
    return (int)ms;
}

static int64_t make_deadline(int timeout_ms)
{
    if (timeout_ms < 0) {
        return -1;
    }
    return nb_platform_time_monotonic_ns() +
           (int64_t)timeout_ms * INT64_C(1000000);
}

static nb_status map_poll_error(int saved, nb_err *err)
{
    nb_status status = saved == EINVAL ? NB_ERR_INVAL : NB_ERR_OS;
    nb_err_set_os(err, status, saved, "poll failed: %s", strerror(saved));
    return status;
}

nb_status nb_platform_poll(nb_poll_item *items, size_t count, int timeout_ms,
                           size_t *ready, nb_err *err)
{
    if (ready) {
        *ready = 0;
    }
    if (!items && count != 0) {
        nb_err_set(err, NB_ERR_INVAL, "items are required");
        return NB_ERR_INVAL;
    }
    if (count == 0) {
        /* No fd can ever become ready; an infinite wait has no exit. */
        if (timeout_ms < 0) {
            nb_err_set(err, NB_ERR_INVAL, "cannot wait forever with no items");
            return NB_ERR_INVAL;
        }
        if (timeout_ms > 0) {
            nb_platform_sleep_ms((uint32_t)timeout_ms);
        }
        nb_err_set(err, NB_ERR_TIMEOUT, "poll timed out");
        return NB_ERR_TIMEOUT;
    }
    if (count > (SIZE_MAX / sizeof(struct pollfd))) {
        nb_err_set(err, NB_ERR_RANGE, "too many poll items");
        return NB_ERR_RANGE;
    }

    struct pollfd stack_items[NB_POLL_STACK_ITEMS];
    struct pollfd *fds = stack_items;
    if (count > NB_POLL_STACK_ITEMS) {
        fds = nb_try_alloc(count * sizeof(struct pollfd));
        if (!fds) {
            nb_err_set(err, NB_ERR_NOMEM, "out of memory polling %zu items",
                       count);
            return NB_ERR_NOMEM;
        }
    }

    for (size_t i = 0; i < count; i++) {
        fds[i].fd = items[i].fd;
        fds[i].events = 0;
        if (items[i].want_read) {
            fds[i].events |= POLLIN;
        }
        if (items[i].want_write) {
            fds[i].events |= POLLOUT;
        }
        fds[i].revents = 0;
    }

    int64_t deadline = make_deadline(timeout_ms);
    int rc;
    for (;;) {
        int wait = remaining_ms(deadline);
        if (wait == 0 && timeout_ms != 0) {
            /* A timed wait that expired between iterations. */
            if (fds != stack_items) {
                nb_free(fds, count * sizeof(struct pollfd));
            }
            nb_err_set(err, NB_ERR_TIMEOUT, "poll timed out");
            return NB_ERR_TIMEOUT;
        }
        rc = poll(fds, (nfds_t)count, wait);
        if (rc >= 0 || errno != EINTR) {
            break;
        }
        if (timeout_ms == 0) {
            rc = 0;
            break;
        }
    }

    if (rc < 0) {
        int saved = errno;
        if (fds != stack_items) {
            nb_free(fds, count * sizeof(struct pollfd));
        }
        return map_poll_error(saved, err);
    }

    size_t ready_count = 0;
    for (size_t i = 0; i < count; i++) {
        short revents = fds[i].revents;
        if (revents == 0) {
            continue;
        }
        ready_count++;
        /* POLLHUP still means a read drains and then reports EOF. */
        items[i].ready_read = (revents & (POLLIN | POLLHUP)) != 0;
        items[i].ready_write = (revents & POLLOUT) != 0;
        items[i].error = (revents & (POLLERR | POLLNVAL)) != 0;
    }
    if (fds != stack_items) {
        nb_free(fds, count * sizeof(struct pollfd));
    }
    if (ready) {
        *ready = ready_count;
    }
    if (ready_count == 0) {
        nb_err_set(err, NB_ERR_TIMEOUT, "poll timed out");
        return NB_ERR_TIMEOUT;
    }
    return NB_OK;
}
