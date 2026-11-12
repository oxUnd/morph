#ifndef MORPH_CLI_PRESENTATION_PRESENTER_STATE_H
#define MORPH_CLI_PRESENTATION_PRESENTER_STATE_H

#include "sapi/cli/presentation/markdown.h"
#include "mcp/mcp.h"
#include "util/buf.h"
#include "util/array.h"
#include "util/strmap.h"
#include "util/utf8.h"

struct morph_md_kitty;

/* Owned and initialized by presenter.c; used only by rendering adapters. */
struct cli_presenter_state {
	int event_stream_kind;
	int event_stream_has_delta;
	int event_stream_complete;
	int event_stream_visible;
	size_t reasoning_stream_offset;
	int final_rendered;
	int native_content_started;
	int native_content_visible;
	int markdown_stream_kind;
	int markdown_stream_visible;
	struct cli_markdown_output markdown_output;
	int mcp_tree_active;
	char mcp_tree_server[MCP_NAME_MAX];
	morph_buf_t event_stream;
	morph_array_t agent_receipts;
	morph_buf_t native_content;
	morph_buf_t markdown_stream_text;
	struct utf8_terminal_sanitizer event_stream_sanitizer;
	struct utf8_terminal_sanitizer markdown_stream_sanitizer;
	morph_strmap_t rendered_artifacts;
	morph_strmap_t announced_artifacts;
	struct morph_md_kitty *markdown_stream;
};

#endif
