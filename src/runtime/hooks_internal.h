#ifndef MORPH_RUNTIME_HOOKS_INTERNAL_H
#define MORPH_RUNTIME_HOOKS_INTERNAL_H

#include "runtime/hooks.h"
#include "exec/process.h"
#include "util/buf.h"

#include <regex.h>

/* Catalog/trust is persistent; executor state belongs to one dispatch. */
#define HOOK_LIMIT 128
#define HOOK_OUTPUT_MAX 65536u
#define HOOK_INPUT_MAX (1024u * 1024u)

struct hook_entry {
	char id[65];
	char digest[65];
	char *source;
	char *event;
	char *matcher;
	char *command;
	char *status_message;
	char *definition;
	char *previous;
	char *status;
	const char *diagnostic;
	int supported;
	int trusted;
	int recorded;
	int disabled;
	int project;
	int timeout;
	int context_limit;
	regex_t regex;
	int regex_ready;
};

struct hook_run {
	struct hook_entry *entry;
	char session[PROCESS_SESSION_ID_MAX];
	morph_buf_t out;
	morph_buf_t err;
	int done;
	int rc;
	int exit_code;
	uint64_t duration_ms;
};

const char *hook_entry_state(struct hook_manager *manager, struct hook_entry *entry);
const char *hook_json_string(const cJSON *obj, const char *key);
int hook_decode_output(struct hook_manager *manager, struct hook_run *run, const char *event,
		       morph_buf_t *context, morph_buf_t *reason, cJSON **updated, int *blocked,
		       int *ask, int *terminate);
int hook_event_ignores_matcher(const char *event);
int hook_json_unique_fields(const cJSON *value, int depth);
int hook_refresh_trust(struct hook_manager *manager, struct hook_entry *entry);

#endif
