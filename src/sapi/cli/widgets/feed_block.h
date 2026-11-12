#ifndef MORPH_CLI_WIDGETS_FEED_BLOCK_H
#define MORPH_CLI_WIDGETS_FEED_BLOCK_H

#include "agent/agent_control.h"
#include "util/buf.h"

struct cli_application;
struct cli_transcript_tool;

enum cli_feed_block_kind {
	CLI_FEED_BLOCK_REASONING,
	CLI_FEED_BLOCK_TOOL,
	CLI_FEED_BLOCK_AGENT_RECEIPT,
};

/* All content is borrowed for rendering. Components do not own outer spacing. */
struct cli_feed_block {
	enum cli_feed_block_kind kind;
	int join_previous;
	union {
		const char *reasoning;
		const struct cli_transcript_tool *tool;
		struct {
			const char *id;
			enum agent_status status;
		} agent;
	} content;
};

struct cli_feed_block_style {
	int columns;
	int styled;
	int trailing_newlines;
};

int cli_feed_block_leading_rows(int trailing_newlines);
int cli_feed_block_append(morph_buf_t *out, const struct cli_feed_block *block,
			 struct cli_feed_block_style *style);
int cli_feed_block_write(struct cli_application *ctx, const struct cli_feed_block *block);

#endif
