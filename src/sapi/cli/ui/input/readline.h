#ifndef MORPH_CLI_UI_INPUT_READLINE_H
#define MORPH_CLI_UI_INPUT_READLINE_H

struct cli_context;

const char *cli_readline_input_prompt(struct cli_context *ctx);
#ifdef HAVE_READLINE
struct cli_command_job;
void cli_readline_run(struct cli_context *, struct cli_command_job *,
		      void (*)(struct cli_context *, struct cli_command_job *));
void cli_readline_configure(struct cli_context *);
void cli_readline_clear_footer(struct cli_context *);
void cli_readline_redisplay(struct cli_context *);
void cli_readline_line_ready(struct cli_context *, char *input);
char *cli_readline_suspend(struct cli_context *, int *point);
void cli_readline_resume(struct cli_context *, char *draft, int point);
void cli_readline_finish_job(struct cli_context *);
void cli_readline_drain_ui(struct cli_context *);
void cli_readline_render_frame(struct cli_context *, int resized);
void cli_readline_details_input(struct cli_context *);
#endif

#endif
