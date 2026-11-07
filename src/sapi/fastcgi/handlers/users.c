/* users.c — install, signup, and current-user quota endpoints */
#define _GNU_SOURCE
#include "handlers.h"
#include "sapi/fastcgi/storage/users.h"
#include "sapi/fastcgi/storage/quota.h"
#include "sapi/fastcgi/storage/tokens.h"
#include "../session_store.h"
#include "../auth.h"
#include "../security.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/json.h"

static char *user_created_json(const char *user_id, const char *username, const char *role)
{
	struct json *root = json_object();
	char *json;

	if (!root)
		return NULL;
	json_add_string(root, "user_id", user_id ? user_id : "");
	json_add_string(root, "username", username ? username : "");
	json_add_string(root, "role", role ? role : "user");
	json = json_stringify(root);
	json_free(root);
	return json;
}

static int read_user_password(request_t *r, char **body_out, struct json **root_out,
			      const char **username, const char **password,
			      const char **signup_code)
{
	char *body = NULL;
	size_t blen = 0;
	struct json *root;
	struct json *u;
	struct json *p;
	struct json *code;

	{
		int rc = fcgi_read_body(r, &body, &blen);
		if (rc != 0)
			return rc;
	}
	root = json_parse_prefix(body ? body : "{}", NULL, NULL);
	if (!json_is_object_value(root)) {
		json_free(root);
		free(body);
		return -EINVAL;
	}
	u = json_member_ci(root, "username");
	p = json_member_ci(root, "password");
	code = json_member_ci(root, "signup_code");
	if (!json_is_string_value(u) || !json_string_value(u) || !json_is_string_value(p) ||
	    !json_string_value(p)) {
		json_free(root);
		free(body);
		return -EINVAL;
	}
	*body_out = body;
	*root_out = root;
	*username = json_string_value(u);
	*password = json_string_value(p);
	*signup_code = json_is_string_value(code) ? json_string_value(code) : NULL;
	return 0;
}

void handle_install(request_t *r)
{
	char *body = NULL;
	struct json *root = NULL;
	const char *username = NULL;
	const char *password = NULL;
	const char *signup_code = NULL;
	char username_buf[64] = {0};
	char user_id[64] = {0};
	char *json;
	int rc;

	if (!store_setup_required(r->store)) {
		reply_json(r, 409, "{\"error\":\"already_installed\"}");
		return;
	}
	rc = read_user_password(r, &body, &root, &username, &password, &signup_code);
	(void)signup_code;
	if (rc < 0) {
		if (rc == -EINVAL)
			reply_400(r, "invalid json");
		else
			reply_body_error(r, rc);
		return;
	}
	snprintf(username_buf, sizeof(username_buf), "%s", username);
	rc = store_create_user(r->store, username, password, "admin", user_id);
	json_free(root);
	free(body);
	if (rc == -EEXIST) {
		reply_json(r, 409, "{\"error\":\"user_exists\"}");
		return;
	}
	if (rc == -EINVAL) {
		reply_400(r, "invalid username or password");
		return;
	}
	if (rc != 0) {
		reply_500(r, "install failed");
		return;
	}
	json = user_created_json(user_id, username_buf, "admin");
	if (!json) {
		reply_500(r, "oom");
		return;
	}
	reply_201_json(r, json);
	free(json);
}

void handle_signup(request_t *r)
{
	const char *allow = getenv("MORPH_FCGI_ALLOW_SIGNUP");
	const char *required_code = getenv("MORPH_FCGI_SIGNUP_CODE");
	char *body = NULL;
	struct json *root = NULL;
	const char *username = NULL;
	const char *password = NULL;
	const char *signup_code = NULL;
	char username_buf[64] = {0};
	char user_id[64] = {0};
	char *json;
	int rc;

	if (store_setup_required(r->store)) {
		reply_json(r, 403, "{\"error\":\"setup_required\"}");
		return;
	}
	if (!allow || strcmp(allow, "1") != 0) {
		reply_json(r, 403, "{\"error\":\"signup_disabled\"}");
		return;
	}
	rc = read_user_password(r, &body, &root, &username, &password, &signup_code);
	if (rc < 0) {
		if (rc == -EINVAL)
			reply_400(r, "invalid json");
		else
			reply_body_error(r, rc);
		return;
	}
	if (required_code && *required_code &&
	    (!signup_code || strcmp(signup_code, required_code) != 0)) {
		json_free(root);
		free(body);
		reply_json(r, 403, "{\"error\":\"invalid_signup_code\"}");
		return;
	}
	snprintf(username_buf, sizeof(username_buf), "%s", username);
	rc = store_create_user(r->store, username, password, "user", user_id);
	json_free(root);
	free(body);
	if (rc == -EEXIST) {
		reply_json(r, 409, "{\"error\":\"user_exists\"}");
		return;
	}
	if (rc == -EINVAL) {
		reply_400(r, "invalid username or password");
		return;
	}
	if (rc != 0) {
		reply_500(r, "signup failed");
		return;
	}
	json = user_created_json(user_id, username_buf, "user");
	if (!json) {
		reply_500(r, "oom");
		return;
	}
	reply_201_json(r, json);
	free(json);
}

void handle_me_quota(request_t *r)
{
	char *json = NULL;
	int rc = store_user_quota_json(r->store, r->user_id, &json);
	if (rc != 0) {
		reply_500(r, "quota failed");
		return;
	}
	reply_200_json(r, json);
	free(json);
}

void handle_login(request_t *r)
{
	char username_buf[128] = {0};
	char password_buf[256] = {0};
	struct fcgi_user user = {0};
	char token[64] = {0};
	struct json *obj = NULL;
	char *json = NULL;
	int64_t expires;
	int rc;

	if (!fcgi_basic_decode(r->auth_hdr, username_buf, sizeof(username_buf), password_buf,
			       sizeof(password_buf))) {
		reply_401(r);
		return;
	}
	if (!store_verify_user(r->store, username_buf, password_buf, &user)) {
		reply_401(r);
		return;
	}

	rc = login_token_create(user.user_id, user.username, user.role, 24, token);
	if (rc != 0) {
		reply_500(r, "token create failed");
		return;
	}

	expires = (int64_t)time(NULL) + 24 * 3600;
	auth_token_created(r->auth, token, user.user_id, user.username, user.role, expires);
	obj = json_object();
	if (!obj) {
		reply_500(r, "oom");
		return;
	}
	json_add_string(obj, "token", token);
	json_add_string(obj, "user_id", user.user_id);
	json_add_string(obj, "username", user.username);
	json_add_string(obj, "role", user.role);
	json_add_number(obj, "expires_at", (double)expires);
	json = json_stringify(obj);
	json_free(obj);
	if (!json) {
		reply_500(r, "oom");
		return;
	}
	reply_200_json(r, json);
	free(json);
}

void handle_logout(request_t *r)
{
	if (r->auth_hdr && strncmp(r->auth_hdr, "Bearer ", 7) == 0) {
		auth_token_revoked(r->auth, r->auth_hdr + 7);
		login_token_revoke(r->auth_hdr + 7);
	}
	reply_204(r);
}
