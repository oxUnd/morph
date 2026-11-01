#ifndef MORPH_CLI_PAGES_CHAT_H
#define MORPH_CLI_PAGES_CHAT_H
#include <morph/tui/application.h>
struct cli_application;
struct cli_command_job;
struct cli_chat_page;
#ifdef HAVE_READLINE
int cli_chat_create(struct cli_application *, struct cli_command_job *, struct cli_chat_page **out);
struct morph_tui_application_options cli_chat_options(struct cli_chat_page *);
struct morph_tui_controller cli_chat_controller(struct cli_chat_page *);
void cli_chat_job_finished(struct cli_chat_page *, int result);
void cli_chat_destroy(struct cli_chat_page *, int result);
#endif
#endif
