#ifndef MORPH_CLI_RECAP_H
#define MORPH_CLI_RECAP_H

#include "runtime/recap.h"
#include "event/event.h"
#include <stdint.h>

#define CLI_RECAP_DELAY_MS (30 * 60 * 1000)
#define CLI_RECAP_RETRY_MS 30000
#define CLI_RECAP_SESSION_CACHE_MAX 32

struct cli_context;

struct cli_recap_state {
	struct runtime_recap_progress progress;
	struct runtime_recap_progress request;
	int64_t unfocused_since;
	int64_t last_finished;
	int64_t next_poll;
	int64_t retry_at;
	int last_recapped_turns;
	int has_recap;
	int retried;
	int pending;
	int automatic;
	int result_code;
	struct {
		int64_t session_id;
		int completed_turns;
		uint64_t used;
	} seen[CLI_RECAP_SESSION_CACHE_MAX];
	uint64_t seen_clock;
};

int64_t cli_recap_now(void);
int cli_recap_due(const struct cli_recap_state *state, int64_t now);
void cli_recap_focus(struct cli_context *ctx, int focused, int64_t now);
void cli_recap_tick(struct cli_context *ctx, int64_t now);
void cli_recap_turn_finished(struct cli_context *ctx, int64_t now);
int cli_recap_start(struct cli_context *ctx, int automatic);
void cli_recap_event(struct cli_context *ctx, const struct morph_event *event);

#endif
