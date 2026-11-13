#include "nb/poll.h"

#include "platform/nb_platform.h"

nb_status nb_poll(nb_poll_item *items, size_t count, int timeout_ms,
                  size_t *ready, nb_err *err)
{
    if (ready) {
        *ready = 0;
    }
    if (!items && count != 0) {
        nb_err_set(err, NB_ERR_INVAL, "items are required");
        return NB_ERR_INVAL;
    }
    for (size_t i = 0; i < count; i++) {
        if (items[i].fd < 0) {
            nb_err_set(err, NB_ERR_INVAL, "invalid fd at index %zu", i);
            return NB_ERR_INVAL;
        }
        items[i].ready_read = false;
        items[i].ready_write = false;
        items[i].error = false;
    }
    return nb_platform_poll(items, count, timeout_ms, ready, err);
}
