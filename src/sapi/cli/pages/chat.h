#ifndef MORPH_CLI_PAGES_CHAT_H
#define MORPH_CLI_PAGES_CHAT_H

#include <nothing/tui/application.h>
#include <nothing/tui/choice_prompt.h>

struct cli_application;
struct cli_transcript_tool;
struct morph_event;
struct cli_feed_block;
struct runtime_mcp_status;
struct cli_mcp_tree_item;
struct cli_skill_list_row;
struct nt_view;
struct nt_builder;

struct nt_application_options cli_chat_options(void);
int cli_chat_present(struct cli_application *);
int cli_chat_active(const struct cli_application *);
int cli_chat_output(struct cli_application *, const char *text);
int cli_chat_media(struct cli_application *, const char *type, const char *path);
int cli_chat_skills(struct cli_application *, const struct cli_skill_list_row *, size_t count);
int cli_chat_mcp_servers(struct cli_application *, const struct runtime_mcp_status *, size_t count);
int cli_chat_mcp_items(struct cli_application *, const char *kind, const char *server,
	const struct cli_mcp_tree_item *, size_t count);
int cli_chat_command_input(struct cli_application *, const char *text);
void cli_chat_command_end(struct cli_application *);
int cli_chat_message(struct cli_application *, int kind, const char *text);
int cli_chat_react_event(struct cli_application *, const struct morph_event *);
int cli_chat_tool_changed(struct cli_application *, const struct cli_transcript_tool *);
int cli_chat_block(struct cli_application *, const struct cli_feed_block *);
void cli_chat_activity(struct cli_application *, const char *text);
void cli_chat_turn_begin(struct cli_application *);
void cli_chat_turn_end(struct cli_application *, int result);
int cli_chat_reset(struct cli_application *);
int cli_chat_yield_input(struct cli_application *);
int cli_chat_prompt_choice(struct cli_application *, int rows, struct nt_selection_model *,
			   struct nt_choice_list_state *, const struct nt_choice_prompt_options *,
			   FILE *input, char **custom);
int cli_chat_panel_active(const struct cli_application *);
typedef struct nt_view *(*cli_chat_panel_build_fn)(struct nt_builder *, struct nt_view *, void *);
typedef void (*cli_chat_panel_complete_fn)(struct cli_application *, void *, int);
/* Successful mounting transfers data to complete, called once after the panel's native views
 * and active listeners retire. */
int cli_chat_panel_mount(struct cli_application *, cli_chat_panel_build_fn, void *data,
			 cli_chat_panel_complete_fn complete);
int cli_chat_panel_close(struct cli_application *, int result);
void cli_chat_panel_set_refresh(struct cli_application *,
				int (*refresh)(void *, const struct nt_event *));
void cli_chat_transcript_reset(struct cli_application *);
int cli_chat_files_show(struct cli_application *, const char *url);
int cli_chat_files_event(struct cli_application *, const struct morph_event *);

#endif
