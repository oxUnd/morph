#ifndef MORPH_FCGI_STORAGE_TOKENS_H
#define MORPH_FCGI_STORAGE_TOKENS_H

#include <stdint.h>

struct session_store;

/* login tokens (/tmp file-backed) */
int login_token_create(const char *user_id, const char *username, const char *role, int ttl_hours,
		       char out_token[64]);
int login_token_verify(const char *token, char out_user_id[64], char out_username[64],
		       char out_role[24]);
void login_token_revoke(const char *token);
int login_token_prune(void);

#endif
