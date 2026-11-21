#ifndef MORPH_CLI_SERVICES_SESSIONS_H
#define MORPH_CLI_SERVICES_SESSIONS_H

#include <stdint.h>
#include <stddef.h>

struct cli_application;
struct session;
struct cJSON;

int cli_sessions_init(struct cli_application *ctx);
void cli_sessions_cleanup(struct cli_application *ctx);
int cli_sessions_drain(struct cli_application *ctx);
int cli_sessions_submit(struct cli_application *ctx, const char *input,
	const char *stored_input);
int cli_sessions_execute(struct cli_application *ctx, const char *input,
	const char *stored_input);
void cli_sessions_stop(struct cli_application *ctx);
int cli_sessions_expedite(struct cli_application *ctx);
int cli_sessions_prompts_copy_since(struct cli_application *ctx, char **items,
	size_t capacity, size_t *total, int64_t *revision);
int cli_sessions_select(struct cli_application *ctx, int64_t id);
int cli_sessions_create(struct cli_application *ctx, const char *name,
	int auto_named, struct session *out);
int cli_sessions_delete(struct cli_application *ctx, int64_t id);
int cli_sessions_interrupt(struct cli_application *ctx, int preserve_processes);
int cli_sessions_active(const struct cli_application *ctx);
int cli_sessions_running_count(const struct cli_application *ctx);
int cli_sessions_open_count(const struct cli_application *ctx);
int64_t cli_sessions_current_id(const struct cli_application *ctx);
int cli_sessions_cycle(struct cli_application *ctx, int direction);
int cli_sessions_is_running(const struct cli_application *ctx, int64_t id);
int cli_sessions_needs_input(const struct cli_application *ctx, int64_t id);
/* Worker callers wait while the UI owner continues consuming runtime messages.
 * The output may contain a partial report on failure and is always caller-owned. */
int cli_sessions_request_current(struct cli_application *ctx, const char *method,
	const struct cJSON *params, struct cJSON **out);
int cli_sessions_request(struct cli_application *ctx, const char *method,
	const struct cJSON *params, struct cJSON **out);

#endif
