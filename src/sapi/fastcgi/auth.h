/* auth.h — authentication / authorization */
#ifndef MORPH_FCGI_AUTH_H
#define MORPH_FCGI_AUTH_H

#include "fcgi_io.h"

struct fcgi_auth *auth_create(const char *trust_header);
void auth_destroy(struct fcgi_auth *auth);
int  auth_check(request_t *r);
void auth_token_created(struct fcgi_auth *ctx, const char *token, const char *user_id,
			const char *username, const char *role, int64_t expires_at);
void auth_token_revoked(struct fcgi_auth *ctx, const char *token);

#endif
