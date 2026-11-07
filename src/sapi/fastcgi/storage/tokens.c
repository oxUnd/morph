#include "sapi/fastcgi/storage/tokens.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "cJSON.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include "sapi/fastcgi/security.h"
#include "util/file.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- login tokens (/tmp file-backed) ---- */

#define SESS_DIR "/tmp/morph-sess"
#define SESS_TOKEN_TTL_HOURS 24

static int ensure_sess_dir(void)
{
	struct stat st;
	if (stat(SESS_DIR, &st) == 0 && S_ISDIR(st.st_mode))
		return 0;
	if (mkdir(SESS_DIR, 0700) != 0 && errno != EEXIST)
		return morph_error_from_errno(errno);
	return 0;
}

int login_token_create(const char *user_id, const char *username, const char *role, int ttl_hours,
		       char out_token[64])
{
	char token[64];
	char path[PATH_MAX];
	int64_t now = fcgi_store_now();
	int64_t expires = now + (int64_t)(ttl_hours > 0 ? ttl_hours : SESS_TOKEN_TTL_HOURS) * 3600;
	cJSON *obj = NULL;
	char *json = NULL;
	FILE *fp = NULL;
	int rc;

	rc = ensure_sess_dir();
	if (rc < 0)
		return rc;
	rc = fcgi_random_id("sess_", token, sizeof(token));
	if (rc < 0)
		return rc;

	obj = cJSON_CreateObject();
	if (!obj)
		return -ENOMEM;
	cJSON_AddStringToObject(obj, "user_id", user_id ? user_id : "");
	cJSON_AddStringToObject(obj, "username", username ? username : "");
	cJSON_AddStringToObject(obj, "role", role ? role : "user");
	cJSON_AddNumberToObject(obj, "expires_at", (double)expires);
	json = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (!json)
		return -ENOMEM;

	if (file_path_join_append(path, sizeof(path), SESS_DIR, token, ".json") != 0) {
		free(json);
		return -ENAMETOOLONG;
	}
	fp = fopen(path, "wx");
	if (!fp) {
		int err = errno;
		free(json);
		return -err;
	}
	fwrite(json, 1, strlen(json), fp);
	fclose(fp);
	free(json);

	snprintf(out_token, 64, "%s", token);
	return 0;
}

int login_token_verify(const char *token, char out_user_id[64], char out_username[64],
		       char out_role[24])
{
	char path[PATH_MAX];
	char buf[1024];
	FILE *fp;
	size_t n;
	cJSON *obj = NULL;
	const char *s;
	int64_t expires;
	int valid = 0;

	if (!token || !*token)
		return 0;
	if (strchr(token, '/') || strstr(token, ".."))
		return 0;
	if (file_path_join_append(path, sizeof(path), SESS_DIR, token, ".json") != 0)
		return 0;

	fp = fopen(path, "r");
	if (!fp)
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	fclose(fp);
	if (n == 0)
		return 0;
	buf[n] = '\0';

	obj = cJSON_Parse(buf);
	if (!obj)
		return 0;

	expires = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(obj, "expires_at"));
	if (expires > fcgi_store_now()) {
		valid = 1;
		s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "user_id"));
		if (s && out_user_id)
			snprintf(out_user_id, 64, "%s", s);
		s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "username"));
		if (s && out_username)
			snprintf(out_username, 64, "%s", s);
		s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "role"));
		if (s && out_role)
			snprintf(out_role, 24, "%s", s);
	}
	cJSON_Delete(obj);

	if (!valid) {
		unlink(path);
		return 0;
	}
	return 1;
}

void login_token_revoke(const char *token)
{
	char path[PATH_MAX];
	if (!token || !*token)
		return;
	if (strchr(token, '/') || strstr(token, ".."))
		return;
	if (file_path_join_append(path, sizeof(path), SESS_DIR, token, ".json") != 0)
		return;
	unlink(path);
}

int login_token_prune(void)
{
	DIR *dir = opendir(SESS_DIR);
	struct dirent *entry;

	if (!dir)
		return 0;
	while ((entry = readdir(dir)) != NULL) {
		char path[PATH_MAX];
		char buf[1024];
		FILE *fp;
		size_t n;
		cJSON *obj;
		int64_t expires;

		if (entry->d_name[0] == '.')
			continue;
		if (file_path_join_append(path, sizeof(path), SESS_DIR, entry->d_name, NULL) != 0)
			continue;
		fp = fopen(path, "r");
		if (!fp)
			continue;
		n = fread(buf, 1, sizeof(buf) - 1, fp);
		fclose(fp);
		if (n == 0)
			continue;
		buf[n] = '\0';
		obj = cJSON_Parse(buf);
		if (!obj)
			continue;
		expires = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(obj, "expires_at"));
		cJSON_Delete(obj);
		if (expires > 0 && expires <= fcgi_store_now())
			(void)unlink(path);
	}
	closedir(dir);
	return 0;
}
