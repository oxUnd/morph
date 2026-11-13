#ifndef MORPH_CLI_PRESENTATION_PRESENTER_STATE_H
#define MORPH_CLI_PRESENTATION_PRESENTER_STATE_H

#include "sapi/cli/presentation/markdown.h"
#include "mcp/mcp.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/map.h"
#include "nb/utf8.h"
#include <morph/tui/terminal/text.h>

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
	nb_buf event_stream;
	nb_buf native_content;
	nb_buf markdown_stream_text;
	struct morph_terminal_sanitizer event_stream_sanitizer;
	struct morph_terminal_sanitizer markdown_stream_sanitizer;
	nb_map *rendered_artifacts;
	nb_map *announced_artifacts;
	struct morph_md_kitty *markdown_stream;
};

#endif
