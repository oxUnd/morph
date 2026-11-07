#ifndef MORPH_FCGI_SESSION_STORE_INTERNAL_H
#define MORPH_FCGI_SESSION_STORE_INTERNAL_H

#include "sapi/fastcgi/session_store.h"
#include <stdint.h>

int64_t fcgi_store_now(void);
/* A transaction holds the same connection mutex until store_transaction_end. */
int store_transaction_begin(struct session_store *store);
int store_transaction_end(struct session_store *store, int result);
void store_events_cleanup(struct session_store *store);

#endif
