#ifndef MORPH_CLI_ADAPTERS_INPUT_READLINE_H
#define MORPH_CLI_ADAPTERS_INPUT_READLINE_H

struct cli_application;

const char *cli_readline_input_prompt(struct cli_application *ctx);
#ifdef HAVE_READLINE
struct cli_command_job;
void cli_readline_run(struct cli_application *, struct cli_command_job *,
		      void (*)(struct cli_application *, struct cli_command_job *));
/* Opening consumes a non-NULL draft after restoring it. */
void cli_readline_editor_open(struct cli_application *, char *draft, int point);
void cli_readline_editor_close(void);
void cli_readline_editor_read(struct cli_application *);
/* A negative point places the cursor at the end. */
void cli_readline_editor_replace(struct cli_application *, const char *text, int point);
void cli_readline_editor_remember(const char *input);
void cli_readline_configure(struct cli_application *);
void cli_readline_clear_footer(struct cli_application *);
void cli_readline_redisplay(struct cli_application *);
void cli_readline_line_ready(struct cli_application *, char *input);
char *cli_readline_suspend(struct cli_application *, int *point);
void cli_readline_resume(struct cli_application *, char *draft, int point);
void cli_readline_finish_job(struct cli_application *);
void cli_readline_drain_ui(struct cli_application *);
void cli_readline_render_frame(struct cli_application *, int resized);
#endif

#endif
