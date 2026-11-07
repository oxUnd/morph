#ifndef MORPH_CLI_ADAPTERS_INPUT_READLINE_H
#define MORPH_CLI_ADAPTERS_INPUT_READLINE_H

struct cli_application;

const char *cli_readline_input_prompt_borrow(struct cli_application *ctx);
#ifdef HAVE_READLINE
struct cli_command_job;
void cli_readline_run(struct cli_application *, struct cli_command_job *,
		      void (*)(struct cli_application *, struct cli_command_job *));
/* Takes a malloc-owned draft when provided; success clears its source slot. */
void cli_readline_editor_open_take(struct cli_application *, char **draft, int point);
void cli_readline_editor_close(void);
void cli_readline_editor_read(struct cli_application *);
/* A negative point places the cursor at the end. */
void cli_readline_editor_replace(struct cli_application *, const char *text, int point);
void cli_readline_editor_remember(const char *input);
void cli_readline_configure(struct cli_application *);
void cli_readline_clear_footer(struct cli_application *);
void cli_readline_redisplay(struct cli_application *);
/* Fixed Readline callback ABI: input is handed to this adapter as an owner.
 * The adapter consumes it even when no application is available. */
void cli_readline_line_ready(struct cli_application *, char *input);
char *cli_readline_suspend_copy(struct cli_application *, int *point);
/* Restores and takes a malloc-owned draft, clearing its source slot.
 * An invalid application or empty source leaves the source untouched. */
void cli_readline_resume_take(struct cli_application *, char **draft, int point);
void cli_readline_finish_job(struct cli_application *);
void cli_readline_drain_ui(struct cli_application *);
void cli_readline_render_frame(struct cli_application *, int resized);
#endif

#endif
