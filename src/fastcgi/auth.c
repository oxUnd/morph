#include "auth.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "security.h"
#include "session_store.h"

static char g_trust_hdr[64] = {0};

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

static struct token_entry *g_token_cache[TOKEN_CACHE_SIZE];
static pthread_mutex_t     g_token_mu = PTHREAD_MUTEX_INITIALIZER;

static unsigned token_hash(const char *s)
{
	unsigned h = 5381;
	while (*s) h = h * 33 + (unsigned char)*s++;
	return h;
}

static struct token_entry *cache_get(const char *tok)
{
	unsigned idx = token_hash(tok) % TOKEN_CACHE_SIZE;
	struct token_entry *e;
	for (e = g_token_cache[idx]; e; e = e->next)
		if (strcmp(e->token, tok) == 0)
			return e;
	return NULL;
}

static void cache_put(const char *token, const char *user_id,
		      const char *username, const char *role,
		      int64_t expires_at)
{
	unsigned idx = token_hash(token) % TOKEN_CACHE_SIZE;
	struct token_entry *e;

	for (e = g_token_cache[idx]; e; e = e->next) {
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
	e->next = g_token_cache[idx];
	g_token_cache[idx] = e;
}

static void cache_del(const char *tok)
{
	unsigned idx = token_hash(tok) % TOKEN_CACHE_SIZE;
	struct token_entry **pp = &g_token_cache[idx];
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

void auth_init(const char *trust_header)
{
	memset(g_trust_hdr, 0, sizeof(g_trust_hdr));
	if (trust_header) snprintf(g_trust_hdr, sizeof(g_trust_hdr), "%s", trust_header);
}

int auth_check(request_t *r)
{
	if (g_trust_hdr[0] && r->trust_user && r->trust_user[0]) {
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

		pthread_mutex_lock(&g_token_mu);
		struct token_entry *e = cache_get(tok);
		if (e && e->expires_at > now) {
			snprintf(r->user_id, sizeof(r->user_id), "%s", e->user_id);
			snprintf(r->username, sizeof(r->username), "%s", e->username);
			snprintf(r->role, sizeof(r->role), "%s", e->role);
			pthread_mutex_unlock(&g_token_mu);
			return 1;
		}
		pthread_mutex_unlock(&g_token_mu);

		/* cache miss or expired → read file */
		int64_t expires = 0;
		char uid[64] = {0}, uname[64] = {0}, urole[24] = {0};
		if (login_token_verify(tok, uid, uname, urole)) {
			/* re-read file to get expires_at for caching */
			char path[512];
			snprintf(path, sizeof(path), "/tmp/morph-sess/%s.json", tok);
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
			pthread_mutex_lock(&g_token_mu);
			cache_put(tok, uid, uname, urole, expires);
			pthread_mutex_unlock(&g_token_mu);
			return 1;
		}
	}
	return 0;
}

void auth_token_created(const char *token, const char *user_id,
			const char *username, const char *role,
			int64_t expires_at)
{
	pthread_mutex_lock(&g_token_mu);
	cache_put(token, user_id, username, role, expires_at);
	pthread_mutex_unlock(&g_token_mu);
}

void auth_token_revoked(const char *token)
{
	pthread_mutex_lock(&g_token_mu);
	cache_del(token);
	pthread_mutex_unlock(&g_token_mu);
}
