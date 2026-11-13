#include "nb/signal.h"

nb_status nb_signal_listener_start(nb_signal_listener **out, const int *signals, size_t count,
                                   nb_signal_fn callback, void *user_data, nb_err *err)
{
    (void)out;
    (void)signals;
    (void)count;
    (void)callback;
    (void)user_data;
    nb_err_set(err, NB_ERR_UNSUPPORTED, "native signal listeners are unavailable");
    return NB_ERR_UNSUPPORTED;
}

nb_status nb_signal_listener_send(nb_signal_listener *listener, int signal_number, nb_err *err)
{
    (void)listener;
    (void)signal_number;
    nb_err_set(err, NB_ERR_UNSUPPORTED, "native signal listeners are unavailable");
    return NB_ERR_UNSUPPORTED;
}

nb_status nb_signal_listener_stop(nb_signal_listener **owner, nb_err *err)
{
    if (!owner || !*owner) {
        return NB_OK;
    }
    nb_err_set(err, NB_ERR_UNSUPPORTED, "native signal listeners are unavailable");
    return NB_ERR_UNSUPPORTED;
}
