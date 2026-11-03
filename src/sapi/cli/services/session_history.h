#ifndef MORPH_CLI_SERVICES_SESSION_HISTORY_H
#define MORPH_CLI_SERVICES_SESSION_HISTORY_H

struct cli_application;

int cli_session_history_restore(struct cli_application *ctx);
void cli_session_history_print_resume_hint(struct cli_application *ctx);

#endif
