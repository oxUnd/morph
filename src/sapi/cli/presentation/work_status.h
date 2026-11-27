#ifndef MORPH_CLI_PRESENTATION_WORK_STATUS_H
#define MORPH_CLI_PRESENTATION_WORK_STATUS_H

#include <nb/buf.h>
#include <nb/cleanup.h>
#include <stdint.h>
#include <time.h>

/* Trusted SGR text preserves the original transient and completed work rows. */
int cli_work_status_running(nb_buf *out, int columns, int styled, int64_t elapsed_ms,
			    int64_t now_ms, const char *agent_summary);
/* Native animation clocks advance the sweep independently of turn duration. */
int cli_work_status_running_frame(nb_buf *, int columns, int styled, int64_t elapsed_ms,
	int64_t now_ms, int64_t animation_ms, const char *agent_summary);
/* Completed content excludes its component's two-cell left padding. */
int cli_work_status_completed(nb_buf *out, int columns, int styled, int result, int64_t elapsed_ms,
			      time_t finished_at);
int cli_work_status_retry(nb_buf *, int columns, int styled, int attempt, int limit,
	int persistent, int waiting, int64_t remaining_ms, int64_t now_ms);
int cli_work_status_interval_ms(int64_t elapsed_ms, int64_t now_ms);
/* Single-line queue previews share the composer's bounded gray detail rows. */
int cli_work_status_queue(nb_buf *, const char *const *prompts, size_t shown, size_t total,
	int columns, int styled);

#endif
