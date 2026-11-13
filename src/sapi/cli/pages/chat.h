#ifndef MORPH_CLI_PAGES_CHAT_H
#define MORPH_CLI_PAGES_CHAT_H
#include <morph/tui/application.h>
struct cli_application;
struct cli_command_job;
struct cli_chat_page;
struct cli_transcript_tool;
struct morph_event;
struct cli_feed_block;
int cli_chat_create(struct cli_application *, struct cli_command_job *, struct cli_chat_page **out);
struct morph_tui_application_options cli_chat_options(struct cli_chat_page *);
int cli_chat_present(struct cli_chat_page *);
void cli_chat_job_finished(struct cli_chat_page *, int result);
void cli_chat_destroy(struct cli_chat_page *, int result);
int cli_chat_active(const struct cli_application *);
int cli_chat_output(struct cli_application *, const char *text);
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
int cli_chat_flush(struct cli_application *);
void cli_chat_transcript_reset(struct cli_application *);
#endif
