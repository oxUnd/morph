/* auth.h — authentication / authorization */
#ifndef MORPH_FCGI_AUTH_H
#define MORPH_FCGI_AUTH_H

#include "fcgi_io.h"

void auth_init(const char *bearer_secret, const char *trust_header);
int  auth_check(request_t *r);

#endif
