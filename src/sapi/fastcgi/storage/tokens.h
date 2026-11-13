#ifndef MORPH_FCGI_STORAGE_TOKENS_H
#define MORPH_FCGI_STORAGE_TOKENS_H

#include <stdint.h>

struct session_store;

/* Login tokens are /tmp file-backed, atomically published with mode 0600.
 * Records are read completely up to 64 KiB; oversize/malformed records fail
 * verification and are retained by pruning. Only parsed positive expiry
 * times at or before now are pruned. Failed creation leaves out_token intact;
 * verification fills optional expires_out only on successful verification. */
int login_token_create(const char *user_id, const char *username, const char *role, int ttl_hours,
		       char out_token[64]);
int login_token_verify(const char *token, char out_user_id[64], char out_username[64],
		       char out_role[24], int64_t *expires_out);
void login_token_revoke(const char *token);
int login_token_prune(void);

#endif
