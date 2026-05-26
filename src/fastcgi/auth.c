#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_secret[256] = {0};
static char g_trust_hdr[64] = {0};

void auth_init(const char *bearer_secret, const char *trust_header) {
	memset(g_secret, 0, sizeof(g_secret));
	memset(g_trust_hdr, 0, sizeof(g_trust_hdr));
	if (bearer_secret) snprintf(g_secret,    sizeof(g_secret),    "%s", bearer_secret);
	if (trust_header)  snprintf(g_trust_hdr, sizeof(g_trust_hdr), "%s", trust_header);
}

static int eq_ct(const char *a, const char *b) {
	if (!a || !b) return 0;
	size_t la = strlen(a), lb = strlen(b);
	if (la != lb) return 0;
	unsigned diff = 0;
	for (size_t i = 0; i < la; i++) diff |= (unsigned)(a[i] ^ b[i]);
	return diff == 0;
}

int auth_check(request_t *r) {
	if (g_trust_hdr[0] && r->trust_user && r->trust_user[0]) {
		snprintf(r->user_id, sizeof(r->user_id), "%s", r->trust_user);
		return 1;
	}
	if (g_secret[0]) {
		if (!r->auth_hdr) return 0;
		if (strncmp(r->auth_hdr, "Bearer ", 7) != 0) return 0;
		const char *tok = r->auth_hdr + 7;
		if (!eq_ct(tok, g_secret)) return 0;
		snprintf(r->user_id, sizeof(r->user_id), "shared");
		return 1;
	}
	return 0;
}
