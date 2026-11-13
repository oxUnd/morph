#ifndef MORPH_CLI_WIDGETS_FEED_BLOCK_H
#define MORPH_CLI_WIDGETS_FEED_BLOCK_H

#include "agent/agent_control.h"
#include "util/buf.h"
#include "sapi/cli/widgets/notice.h"
#include <morph/tui/view.h>

struct cli_application;
struct cli_transcript_tool;

enum cli_feed_block_kind {
	CLI_FEED_BLOCK_REASONING,
	CLI_FEED_BLOCK_TOOL,
	CLI_FEED_BLOCK_AGENT_RECEIPT,
	CLI_FEED_BLOCK_PLAN,
	CLI_FEED_BLOCK_NOTICE,
};

/* All content is borrowed for rendering. Each component owns its outer box
 * margin; the shared block writer applies it through the TUI box model. */
struct cli_feed_block {
	enum cli_feed_block_kind kind;
	int join_previous;
	int details;
	struct morph_tui_view_insets margin;
	union {
		const char *reasoning;
		const struct cli_transcript_tool *tool;
		const struct cJSON *plan;
		const struct cli_notice *notice;
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

int cli_feed_block_leading_rows(struct morph_tui_view_insets margin, int trailing_newlines);
int cli_feed_block_append(morph_buf_t *out, const struct cli_feed_block *block,
			 struct cli_feed_block_style *style);
int cli_feed_block_write(struct cli_application *ctx, const struct cli_feed_block *block);
int cli_feed_notice_write(struct cli_application *ctx, enum cli_notice_status status,
			  const char *title, const char *detail, const char *footer);

#endif
