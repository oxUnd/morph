#ifndef MORPH_CLI_PRESENTATION_WORK_STATUS_H
#define MORPH_CLI_PRESENTATION_WORK_STATUS_H

#include "util/buf.h"
#include <stdint.h>
#include <time.h>

/* Trusted SGR text preserves the original transient and completed work rows. */
int cli_work_status_running(morph_buf_t *out, int columns, int styled, int64_t elapsed_ms,
			    int64_t now_ms, const char *agent_summary);
/* Completed content excludes its component's two-cell left padding. */
int cli_work_status_completed(morph_buf_t *out, int columns, int styled, int result,
			      int64_t elapsed_ms, time_t finished_at);
int cli_work_status_interval_ms(int64_t elapsed_ms, int64_t now_ms);

#endif
