/* router.h — route table + dispatcher */
#ifndef MORPH_FCGI_ROUTER_H
#define MORPH_FCGI_ROUTER_H

#include "fcgi_io.h"

/* Dispatch a single request.  Always responds (even on error). */
void router_dispatch(request_t *r);

#endif
