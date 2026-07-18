/* auth.h — authentication / authorization */
#ifndef MORPH_FCGI_AUTH_H
#define MORPH_FCGI_AUTH_H

#include "fcgi_io.h"

void auth_init(const char *trust_header);
int  auth_check(request_t *r);
void auth_token_created(const char *token, const char *user_id,
			const char *username, const char *role,
			int64_t expires_at);
void auth_token_revoked(const char *token);

#endif
