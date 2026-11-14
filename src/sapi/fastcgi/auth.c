#include "auth.h"
#include "nb/cleanup.h"
#include "sapi/fastcgi/storage/users.h"
#include "sapi/fastcgi/storage/tokens.h"

#include "nb/thread.h"
#include "nb/map.h"
#include "nb/queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "security.h"
#include "session_store.h"
#include "nb/file.h"

/* ---- in-memory token cache (hash table) ---- */
#define TOKEN_CACHE_MAX 4096
#define TOKEN_EVICTION_BUCKETS 256

struct token_entry {
	char token[64];
	char user_id[64];
	char username[64];
	char role[24];
	int64_t expires_at;
	nb_queue eviction;
};

struct fcgi_auth {
	char trust_header[64];
	nb_map *tokens;
	nb_queue eviction[TOKEN_EVICTION_BUCKETS];
	nb_mutex *lock;
};

/* Keep the historical eviction order; lookup and ownership belong to nb_map. */
static unsigned token_eviction_bucket(const char *token)
{
	unsigned hash = 5381;
	while (*token)
		hash = hash * 33u + (unsigned char)*token++;
	return hash % TOKEN_EVICTION_BUCKETS;
}

static void token_entry_cleanup(struct token_entry **owner)
{
	free(*owner);
	*owner = NULL;
}

static void token_entry_destroy(void *value, void *context)
{
	(void)context;
	struct token_entry *entry = value;
	token_entry_cleanup(&entry);
}

static struct token_entry *cache_get(struct fcgi_auth *ctx, const char *tok)
{
	return ctx ? nb_map_get_cstr(ctx->tokens, tok) : NULL;
}

static void cache_del(struct fcgi_auth *ctx, const char *tok)
{
	if (!ctx)
		return;
	struct token_entry *entry NB_CLEANUP(token_entry_cleanup) =
		nb_map_del_cstr(ctx->tokens, tok);
	if (entry)
		nb_queue_remove(&entry->eviction);
}

static void cache_put(struct fcgi_auth *ctx, const char *token, const char *user_id,
		      const char *username, const char *role, int64_t expires_at)
{
	struct token_entry *entry = NULL;
	if (!ctx || strlen(token) >= sizeof(entry->token))
		return;
	entry = cache_get(ctx, token);
	if (!entry) {
		struct token_entry *created NB_CLEANUP(token_entry_cleanup) =
			calloc(1, sizeof(*created));
		if (!created)
			return;
		if (nb_map_size(ctx->tokens) >= TOKEN_CACHE_MAX) {
			for (size_t bucket = 0; bucket < TOKEN_EVICTION_BUCKETS; bucket++) {
				if (nb_queue_empty(&ctx->eviction[bucket]))
					continue;
				struct token_entry *victim = NB_QUEUE_DATA(
					nb_queue_head(&ctx->eviction[bucket]), struct token_entry,
						eviction);
				cache_del(ctx, victim->token);
				break;
			}
		}
		if (!nb_map_put_cstr(ctx->tokens, token, created, NULL))
			return;
		entry = created;
		created = NULL;
		nb_queue_insert_head(&ctx->eviction[token_eviction_bucket(token)],
			&entry->eviction);
	}
	snprintf(entry->token, sizeof(entry->token), "%s", token);
	snprintf(entry->user_id, sizeof(entry->user_id), "%s", user_id);
	snprintf(entry->username, sizeof(entry->username), "%s", username);
	snprintf(entry->role, sizeof(entry->role), "%s", role);
	entry->expires_at = expires_at;
}

/* ---- public API ---- */

struct fcgi_auth *auth_create(const char *trust_header)
{
	struct fcgi_auth *ctx = calloc(1, sizeof(*ctx));

	if (!ctx)
		return NULL;
	if (((ctx->lock = nb_mutex_new(0)) ? NB_OK : NB_ERR_NOMEM) != 0) {
		free(ctx);
		return NULL;
	}
	ctx->tokens = nb_map_new_with_cap(256);
	if (!ctx->tokens) {
		nb_mutex_free(ctx->lock);
		free(ctx);
		return NULL;
	}
	nb_map_set_value_dtor(ctx->tokens, token_entry_destroy, NULL);
	for (size_t bucket = 0; bucket < TOKEN_EVICTION_BUCKETS; bucket++)
		nb_queue_init(&ctx->eviction[bucket]);
	if (trust_header)
		snprintf(ctx->trust_header, sizeof(ctx->trust_header), "%s", trust_header);
	return ctx;
}

void auth_destroy(struct fcgi_auth *ctx)
{
	if (!ctx)
		return;
	nb_map_free(ctx->tokens);
	(nb_mutex_free(ctx->lock), ctx->lock = NULL);
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
		if (fcgi_basic_decode(r->auth_hdr, username, sizeof(username), password,
				      sizeof(password)) &&
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
			nb_mutex_lock(ctx->lock);
		struct token_entry *e = cache_get(ctx, tok);
		if (e && e->expires_at > now) {
			snprintf(r->user_id, sizeof(r->user_id), "%s", e->user_id);
			snprintf(r->username, sizeof(r->username), "%s", e->username);
			snprintf(r->role, sizeof(r->role), "%s", e->role);
			if (ctx)
				nb_mutex_unlock(ctx->lock);
			return 1;
		}
		if (ctx)
			nb_mutex_unlock(ctx->lock);

		/* cache miss or expired → read file */
		int64_t expires = 0;
		char uid[64] = {0}, uname[64] = {0}, urole[24] = {0};
		if (login_token_verify(tok, uid, uname, urole, &expires)) {
			snprintf(r->user_id, sizeof(r->user_id), "%s", uid);
			snprintf(r->username, sizeof(r->username), "%s", uname);
			snprintf(r->role, sizeof(r->role), "%s", urole);
			if (ctx)
				nb_mutex_lock(ctx->lock);
			cache_put(ctx, tok, uid, uname, urole, expires);
			if (ctx)
				nb_mutex_unlock(ctx->lock);
			return 1;
		}
	}
	return 0;
}

void auth_token_created(struct fcgi_auth *ctx, const char *token, const char *user_id,
			const char *username, const char *role, int64_t expires_at)
{
	if (ctx)
		nb_mutex_lock(ctx->lock);
	cache_put(ctx, token, user_id, username, role, expires_at);
	if (ctx)
		nb_mutex_unlock(ctx->lock);
}

void auth_token_revoked(struct fcgi_auth *ctx, const char *token)
{
	if (ctx)
		nb_mutex_lock(ctx->lock);
	cache_del(ctx, token);
	if (ctx)
		nb_mutex_unlock(ctx->lock);
}
