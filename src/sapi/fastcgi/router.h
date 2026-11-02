/* router.h — route table + dispatcher */
#ifndef MORPH_FCGI_ROUTER_H
#define MORPH_FCGI_ROUTER_H

#include "fcgi_io.h"

/* Dispatch a single request.  Returns 1 when a long-lived stream took
 * ownership of the FastCGI request, 0 otherwise. */
int router_dispatch(request_t *r);

#endif
