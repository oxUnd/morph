#include "sapi/fastcgi/storage/tokens.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "sapi/fastcgi/security.h"
#include "cJSON.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/file.h"
#include "nb/str.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SESS_DIR "/tmp/morph-sess"
#define SESS_TOKEN_TTL_HOURS 24
#define SESS_RECORD_MAX_BYTES (64u * 1024u)

static int login_token_path(nb_buf *path, const char *token)
{
	if (!token || !*token || strchr(token, '/') || strchr(token, '\\') || strstr(token, ".."))
		return NB_ERR_INVAL;
	int rc = nb_path_join(path, SESS_DIR, token);
	if (rc != NB_OK)
		return rc;
	return nb_buf_append_cstr(path, ".json") ? NB_OK : nb_buf_status(path);
}

static int login_token_load(const char *path, cJSON **out, int64_t *expires)
{
	nb_buf content NB_CLEANUP(nb_buf_destroy) = {0};
	nb_file_info info = {0};
	int rc = nb_file_stat(path, false, &info, NULL);
	if (rc != NB_OK || !info.is_regular)
		return rc != NB_OK ? rc : NB_ERR_FORMAT;
	rc = nb_file_read_all(path, &content, SESS_RECORD_MAX_BYTES, NULL);
	if (rc != NB_OK)
		return rc;
	const char *end = NULL;
	cJSON *object NB_CLEANUP(cJSON_DeleteOwner) =
		cJSON_ParseWithLengthOpts(nb_buf_cstr(&content), content.len + 1u, &end, 1);
	if (!cJSON_IsObject(object) || end != nb_buf_cstr(&content) + content.len)
		return NB_ERR_FORMAT;
	const cJSON *expiry = cJSON_GetObjectItemCaseSensitive(object, "expires_at");
	if (!cJSON_IsNumber(expiry) || !isfinite(expiry->valuedouble) || expiry->valuedouble < 0 ||
	    expiry->valuedouble >= 9223372036854775808.0)
		return NB_ERR_FORMAT;
	*expires = (int64_t)expiry->valuedouble;
	*out = cJSON_StealOwner(&object);
	return NB_OK;
}

int login_token_create(const char *user_id, const char *username, const char *role, int ttl_hours,
		       char out_token[64])
{
	if (!out_token)
		return NB_ERR_INVAL;
	nb_buf path NB_CLEANUP(nb_buf_destroy) = {0};
	cJSON *object NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	char *json NB_CLEANUP(cJSON_FreeTextOwner) = NULL;
	char token[64];
	int64_t expires = fcgi_store_now() +
			  (int64_t)(ttl_hours > 0 ? ttl_hours : SESS_TOKEN_TTL_HOURS) * 3600;
	if (!object || !cJSON_AddStringToObject(object, "user_id", user_id ? user_id : "") ||
	    !cJSON_AddStringToObject(object, "username", username ? username : "") ||
	    !cJSON_AddStringToObject(object, "role", role ? role : "user") ||
	    !cJSON_AddNumberToObject(object, "expires_at", (double)expires))
		return NB_ERR_NOMEM;
	json = cJSON_PrintUnformatted(object);
	if (!json)
		return NB_ERR_NOMEM;
	size_t length = strlen(json);
	if (length > SESS_RECORD_MAX_BYTES)
		return NB_ERR_TOO_BIG;
	int rc = nb_file_mkdir(SESS_DIR, false, NULL);
	if (rc != NB_OK)
		return rc;
	rc = nb_file_set_mode(SESS_DIR, 0700, NULL);
	if (rc != NB_OK)
		return rc;
	rc = fcgi_random_id("sess_", token, sizeof(token));
	if (rc != NB_OK)
		return rc;
	rc = login_token_path(&path, token);
	if (rc != NB_OK)
		return rc;
	bool published = false;
	const nb_file_write_options options = {
		.mode = 0600, .create_only = true, .sync_dir = true, .published = &published};
	rc = nb_file_write_all_atomic(nb_buf_cstr(&path), json, length, &options, NULL);
	if (rc != NB_OK) {
		if (published)
			(void)nb_file_remove(nb_buf_cstr(&path), NULL);
		return rc;
	}
	return nb_str_copy_to(nb_str_from_cstr(token), out_token, 64);
}

int login_token_verify(const char *token, char out_user_id[64], char out_username[64],
		       char out_role[24], int64_t *expires_out)
{
	nb_buf path NB_CLEANUP(nb_buf_destroy) = {0};
	cJSON *object NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	int64_t expires = 0;
	if (login_token_path(&path, token) != NB_OK ||
	    login_token_load(nb_buf_cstr(&path), &object, &expires) != NB_OK)
		return 0;
	if (expires <= fcgi_store_now()) {
		(void)nb_file_remove(nb_buf_cstr(&path), NULL);
		return 0;
	}
	const char *user_id =
		cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "user_id"));
	const char *username =
		cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "username"));
	const char *role = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "role"));
	if (!user_id || !username || !role)
		return 0;
	if (out_user_id)
		(void)nb_str_copy_to(nb_str_from_parts(user_id, NB_MIN(strlen(user_id), 63u)),
				     out_user_id, 64);
	if (out_username)
		(void)nb_str_copy_to(nb_str_from_parts(username, NB_MIN(strlen(username), 63u)),
				     out_username, 64);
	if (out_role)
		(void)nb_str_copy_to(nb_str_from_parts(role, NB_MIN(strlen(role), 23u)), out_role,
				     24);
	if (expires_out)
		*expires_out = expires;
	return 1;
}

void login_token_revoke(const char *token)
{
	nb_buf path NB_CLEANUP(nb_buf_destroy) = {0};
	if (login_token_path(&path, token) == NB_OK)
		(void)nb_file_remove(nb_buf_cstr(&path), NULL);
}

int login_token_prune(void)
{
	nb_dir *directory NB_CLEANUP(nb_dir_cleanup) = nb_dir_open(SESS_DIR, NULL);
	if (!directory)
		return NB_OK;
	const char *name;
	while ((name = nb_dir_next(directory, NULL)) != NULL) {
		if (name[0] == '.')
			continue;
		nb_buf path NB_CLEANUP(nb_buf_destroy) = {0};
		cJSON *object NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		int64_t expires = 0;
		if (nb_path_join(&path, SESS_DIR, name) != NB_OK ||
		    login_token_load(nb_buf_cstr(&path), &object, &expires) != NB_OK)
			continue;
		if (expires > 0 && expires <= fcgi_store_now())
			(void)nb_file_remove(nb_buf_cstr(&path), NULL);
	}
	return NB_OK;
}
