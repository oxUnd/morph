#ifndef MORPH_CLI_WIDGETS_SUBAGENTS_H
#define MORPH_CLI_WIDGETS_SUBAGENTS_H

#include "nb/buf.h"
#include "nb/cleanup.h"
struct cli_application;
struct cli_subagents_thread;
int cli_subagents_transcript(struct cli_application *ctx, const struct cli_subagents_thread *thread,
			     nb_buf *out, int columns, int details);

#endif
