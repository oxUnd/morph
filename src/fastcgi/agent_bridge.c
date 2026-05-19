/* agent_bridge.c — strong-symbol implementations for optional
 * agent-side hooks (see PATCHES.md §3).
 *
 * Provides react_context_create_for_session:
 *   loads config, creates LLM model + tokenizer, populates message
 *   history from the session store, and returns a ready react_context.
 *
 * Linked into morph-fastcgi; overrides the weak symbols in turns.c.
 */
#include "react.h"
#include "session_store.h"
#include "session.h"
#include "agent/context.h"
#include "agent/tokenizer.h"
#include "models/llm.h"
#include "config.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_once_t  g_once    = PTHREAD_ONCE_INIT;
static struct config   g_config;
static struct tokenizer *g_tokenizer = NULL;
static struct model      *g_llm       = NULL;

static void bridge_init_once(void)
{
	config_set_defaults(&g_config);

	/* Try HOME/.morph/config.toml, fall back to cwd */
	const char *home = getenv("HOME");
	if (home) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/.morph/config.toml", home);
		config_load(&g_config, path);
	} else {
		config_load(&g_config, "config.toml");
	}

	g_tokenizer = tokenizer_create(
		g_config.models.text.model,
		g_config.models.text.context_limit);

	if (!g_tokenizer) {
		fprintf(stderr, "fcgi-bridge: tokenizer_create failed\n");
		return;
	}

	/* Resolve API key from env var if needed */
	if (g_config.models.text.api_key_env[0] &&
	    !g_config.models.text.api_key[0]) {
		const char *env = getenv(g_config.models.text.api_key_env);
		if (env)
			snprintf(g_config.models.text.api_key,
				 sizeof(g_config.models.text.api_key),
				 "%s", env);
	}

	g_llm = model_llm_create(
		g_config.models.text.provider,
		g_config.models.text.model,
		g_config.models.text.api_base,
		g_config.models.text.api_key);

	if (!g_llm)
		fprintf(stderr, "fcgi-bridge: model_llm_create failed\n");
}

struct react_context *
react_context_create_for_session(struct session_store *store,
				 const char *session_id,
				 const char *user_id)
{
	(void)user_id;
	pthread_once(&g_once, bridge_init_once);

	struct guardrail_config gcfg = {
		.enabled           = g_config.react.guardrail_enabled,
		.max_retries       = g_config.react.guardrail_max_retries,
		.max_empty_rounds  = g_config.react.guardrail_max_empty_rounds,
	};

	struct compress_config ccfg = {
		.max_context_tokens     = g_config.models.text.context_limit,
		.max_history_rounds    = g_config.context.keep_recent_rounds,
		.summarize_threshold_ratio =
			g_config.context.summarize_threshold_ratio,
		.compress_target_ratio = g_config.context.compress_target_ratio,
	};

	/* tools deliberately NULL — caller registers them */
	struct react_context *ctx = react_context_create(
		NULL, g_tokenizer, &ccfg, &gcfg);
	if (!ctx)
		return NULL;

	ctx->max_iterations       = g_config.react.max_iterations;
	ctx->step_timeout_seconds = g_config.react.step_timeout_seconds;
	ctx->tool_max_retries     = g_config.react.tool_max_retries;
	ctx->llm_model            = g_llm;

	/* Load session from DB to get its numeric id */
	struct session sess;
	if (session_get_by_display_id(&store->db, session_id, &sess) != 0) {
		fprintf(stderr, "fcgi-bridge: session %s not found\n",
			session_id);
		return ctx;  /* return bare context — no history */
	}

	/* Replay message history into react's message_list */
	int msg_count = 0;
	struct message *msgs = message_list(&store->db, sess.id, &msg_count);
	struct message *cur  = msgs;
	while (cur) {
		if (cur->role[0] && cur->content) {
			struct message_list *ml = msg_list_create(
				cur->role, cur->content, cur->token_count);
			if (ml) {
				ml->compressed = cur->compressed;
				msg_list_append(&ctx->messages, ml);
			}
		}
		cur = cur->next;
	}
	message_free_list(msgs);

	return ctx;
}
