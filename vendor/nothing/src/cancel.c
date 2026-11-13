#include "nb/cancel.h"

void nb_cancel_init(nb_cancel *cancel)
{
    if (cancel) {
        nb_atomic_int_init(&cancel->cancelled, 0);
    }
}

void nb_cancel_reset(nb_cancel *cancel)
{
    if (cancel) {
        nb_atomic_int_store(&cancel->cancelled, 0);
    }
}

void nb_cancel_request(nb_cancel *cancel)
{
    if (cancel) {
        nb_atomic_int_store(&cancel->cancelled, 1);
    }
}

bool nb_cancel_requested(const nb_cancel *cancel)
{
    return cancel && nb_atomic_int_load(&cancel->cancelled) != 0;
}
