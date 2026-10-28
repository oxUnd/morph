#include "auth.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "security.h"
#include "session_store.h"
#include "util/file.h"


/* ---- in-memory token cache (hash table) ---- */
#define TOKEN_CACHE_SIZE 256

struct token_entry {
	char            token[64];
	char            user_id[64];
	char            username[64];
	char            role[24];
	int64_t         expires_at;
	struct token_entry *next;
};

struct fcgi_auth {
	char trust_header[64];
	struct token_entry *tokens[TOKEN_CACHE_SIZE];
	pthread_mutex_t lock;
};

static unsigned token_hash(const char *s)
{
	unsigned h = 5381;
	while (*s) h = h * 33 + (unsigned char)*s++;
	return h;
}

static struct token_entry *cache_get(struct fcgi_auth *ctx, const char *tok)
{
	if (!ctx)
		return NULL;
	unsigned idx = token_hash(tok) % TOKEN_CACHE_SIZE;
	struct token_entry *e;
	for (e = ctx->tokens[idx]; e; e = e->next)
		if (strcmp(e->token, tok) == 0)
			return e;
	return NULL;
}

static void cache_put(struct fcgi_auth *ctx, const char *token, const char *user_id,
		      const char *username, const char *role, int64_t expires_at)
{
	if (!ctx)
		return;
	unsigned idx = token_hash(token) % TOKEN_CACHE_SIZE;
	struct token_entry *e;

	for (e = ctx->tokens[idx]; e; e = e->next) {
		if (strcmp(e->token, token) == 0) {
			snprintf(e->user_id, sizeof(e->user_id), "%s", user_id);
			snprintf(e->username, sizeof(e->username), "%s", username);
			snprintf(e->role, sizeof(e->role), "%s", role);
			e->expires_at = expires_at;
			return;
		}
	}

	e = calloc(1, sizeof(*e));
	if (!e) return;
	snprintf(e->token, sizeof(e->token), "%s", token);
	snprintf(e->user_id, sizeof(e->user_id), "%s", user_id);
	snprintf(e->username, sizeof(e->username), "%s", username);
	snprintf(e->role, sizeof(e->role), "%s", role);
	e->expires_at = expires_at;
	e->next = ctx->tokens[idx];
	ctx->tokens[idx] = e;
}

static void cache_del(struct fcgi_auth *ctx, const char *tok)
{
	if (!ctx)
		return;
	unsigned idx = token_hash(tok) % TOKEN_CACHE_SIZE;
	struct token_entry **pp = &ctx->tokens[idx];
	while (*pp) {
		if (strcmp((*pp)->token, tok) == 0) {
			struct token_entry *del = *pp;
			*pp = del->next;
			free(del);
			return;
		}
		pp = &(*pp)->next;
	}
}

/* ---- public API ---- */

struct fcgi_auth *auth_create(const char *trust_header)
{
	struct fcgi_auth *ctx = calloc(1, sizeof(*ctx));

	if (!ctx)
		return NULL;
	if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
		free(ctx);
		return NULL;
	}
	if (trust_header)
		snprintf(ctx->trust_header, sizeof(ctx->trust_header), "%s", trust_header);
	return ctx;
}

void auth_destroy(struct fcgi_auth *ctx)
{
	if (!ctx)
		return;
	for (size_t i = 0; i < TOKEN_CACHE_SIZE; i++) {
		struct token_entry *entry = ctx->tokens[i];

		while (entry) {
			struct token_entry *next = entry->next;

			free(entry);
			entry = next;
		}
	}
	pthread_mutex_destroy(&ctx->lock);
	free(ctx);
}

int auth_check(request_t *r)
{
	struct fcgi_auth *ctx = r->auth;
	if (ctx && ctx->trust_header[0] && r->trust_user && r->trust_user[0]) {
		snprintf(r->user_id, sizeof(r->user_id), "%s", r->trust_user);
		snprintf(r->username, sizeof(r->username), "%s", r->trust_user);
		snprintf(r->role, sizeof(r->role), "user");
		return 1;
	}
	{
		char username[128];
		char password[256];
		struct fcgi_user user = {0};
		if (fcgi_basic_decode(r->auth_hdr, username, sizeof(username),
				      password, sizeof(password)) &&
		    store_verify_user(r->store, username, password, &user)) {
			snprintf(r->user_id, sizeof(r->user_id), "%s", user.user_id);
			snprintf(r->username, sizeof(r->username), "%s", user.username);
			snprintf(r->role, sizeof(r->role), "%s", user.role);
			return 1;
		}
	}
	if (r->auth_hdr && strncmp(r->auth_hdr, "Bearer ", 7) == 0) {
		const char *tok = r->auth_hdr + 7;
		int64_t now = (int64_t)time(NULL);

		if (ctx)
			pthread_mutex_lock(&ctx->lock);
		struct token_entry *e = cache_get(ctx, tok);
		if (e && e->expires_at > now) {
			snprintf(r->user_id, sizeof(r->user_id), "%s", e->user_id);
			snprintf(r->username, sizeof(r->username), "%s", e->username);
			snprintf(r->role, sizeof(r->role), "%s", e->role);
			if (ctx)
				pthread_mutex_unlock(&ctx->lock);
			return 1;
		}
		if (ctx)
			pthread_mutex_unlock(&ctx->lock);

		/* cache miss or expired → read file */
		int64_t expires = 0;
		char uid[64] = {0}, uname[64] = {0}, urole[24] = {0};
		if (login_token_verify(tok, uid, uname, urole)) {
			/* re-read file to get expires_at for caching */
			char path[512];
			if (file_path_join_append(path, sizeof(path),
						  "/tmp/morph-sess", tok,
						  ".json") != 0)
				return 1;
			FILE *fp = fopen(path, "r");
			if (fp) {
				char buf[1024];
				size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
				fclose(fp);
				if (n > 0) {
					buf[n] = '\0';
					/* quick scan for expires_at without full JSON parse */
					const char *p = strstr(buf, "\"expires_at\"");
					if (p) {
						p = strchr(p + 12, ':');
						if (p) expires = strtoll(p + 1, NULL, 10);
					}
				}
			}
			snprintf(r->user_id, sizeof(r->user_id), "%s", uid);
			snprintf(r->username, sizeof(r->username), "%s", uname);
			snprintf(r->role, sizeof(r->role), "%s", urole);
			if (ctx)
				pthread_mutex_lock(&ctx->lock);
			cache_put(ctx, tok, uid, uname, urole, expires);
			if (ctx)
				pthread_mutex_unlock(&ctx->lock);
			return 1;
		}
	}
	return 0;
}

void auth_token_created(struct fcgi_auth *ctx, const char *token, const char *user_id,
			const char *username, const char *role, int64_t expires_at)
{
	if (ctx)
		pthread_mutex_lock(&ctx->lock);
	cache_put(ctx, token, user_id, username, role, expires_at);
	if (ctx)
		pthread_mutex_unlock(&ctx->lock);
}

void auth_token_revoked(struct fcgi_auth *ctx, const char *token)
{
	if (ctx)
		pthread_mutex_lock(&ctx->lock);
	cache_del(ctx, token);
	if (ctx)
		pthread_mutex_unlock(&ctx->lock);
}
