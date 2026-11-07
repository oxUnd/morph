#ifndef MORPH_FCGI_STORAGE_USERS_H
#define MORPH_FCGI_STORAGE_USERS_H

#include <stdint.h>

struct session_store;

struct fcgi_user {
	char user_id[64];
	char username[64];
	char role[24];
};

/* users / setup */
int store_setup_required(struct session_store *s);
int store_create_user(struct session_store *s, const char *username, const char *password,
		      const char *role, char out_user_id[64]);
int store_verify_user(struct session_store *s, const char *username, const char *password,
		      struct fcgi_user *out);

#endif
