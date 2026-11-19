#include "sapi/cli/adapters/input/confirm.h"
#include <nb/err.h>
#include <nb/buf.h>
#include <nb/cleanup.h>
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/pages/question.h"
#include "sapi/cli/presentation/output.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLI_CONFIRM_LINE_MAX 64

struct cli_confirm_answers {
	char **items;
	int count;
};

static void cli_confirm_answers_cleanup(struct cli_confirm_answers *answers)
{
	for (int i = 0; i < answers->count; i++)
		free(answers->items[i]);

	free(answers->items);
}

static enum cli_input_confirm_result cli_confirm_native(struct cli_application *ctx,
							 const char *phrase)
{
	struct cli_confirm_answers answers NB_CLEANUP(cli_confirm_answers_cleanup) = {0};
	nb_buf question NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *choices[] = {"Cancel", "Confirm"};

	cli_output_putchar(ctx, '\n');
	if (!(phrase ? nb_buf_printf(&question, "Type \"%s\" to confirm", phrase)
		     : nb_buf_append_cstr(&question, "Confirm this action?")))
		return CLI_INPUT_CONFIRM_READ_ERROR;

	int rc = cli_question_callback(nb_buf_cstr(&question), phrase ? NULL : choices,
				       phrase ? 0 : 2, "single", 1, 1, NULL,
				       &answers.items, &answers.count, ctx);
	if (rc == NB_ERR_CANCELLED || rc == NB_ERR_INTERRUPTED)
		return CLI_INPUT_CONFIRM_DECLINED;

	if (rc)
		return CLI_INPUT_CONFIRM_READ_ERROR;

	return answers.count == 1 && !strcmp(answers.items[0], phrase ? phrase : "Confirm")
		       ? CLI_INPUT_CONFIRM_ACCEPTED
		       : CLI_INPUT_CONFIRM_DECLINED;
}

/* Returns 1 when a line was read, 0 without a controlling terminal, and a
   negative errno when the terminal could not be read. */
static int cli_confirm_read(char *answer, size_t capacity)
{
	FILE *tty;

	tty = fopen("/dev/tty", "r");
	if (!tty)
		return 0;

	if (!fgets(answer, (int)capacity, tty)) {
		fclose(tty);
		return NB_ERR_IO;
	}

	fclose(tty);
	return 1;
}

enum cli_input_confirm_result cli_input_confirm_yes_no(struct cli_application *ctx)
{
	if (cli_chat_active(ctx))
		return cli_confirm_native(ctx, NULL);

	char answer[CLI_CONFIRM_LINE_MAX];
	int rc = cli_confirm_read(answer, sizeof(answer));

	if (rc == 0)
		return CLI_INPUT_CONFIRM_NO_TERMINAL;

	if (rc < 0)
		return CLI_INPUT_CONFIRM_READ_ERROR;

	return answer[0] == 'y' || answer[0] == 'Y' ? CLI_INPUT_CONFIRM_ACCEPTED
						    : CLI_INPUT_CONFIRM_DECLINED;
}

enum cli_input_confirm_result cli_input_confirm_phrase(struct cli_application *ctx,
						      const char *phrase)
{
	char answer[CLI_CONFIRM_LINE_MAX];
	int rc;

	if (!phrase)
		return CLI_INPUT_CONFIRM_DECLINED;

	if (cli_chat_active(ctx))
		return cli_confirm_native(ctx, phrase);

	rc = cli_confirm_read(answer, sizeof(answer));
	if (rc == 0)
		return CLI_INPUT_CONFIRM_NO_TERMINAL;

	if (rc < 0)
		return CLI_INPUT_CONFIRM_READ_ERROR;

	answer[strcspn(answer, "\r\n")] = '\0';
	return strcmp(answer, phrase) == 0 ? CLI_INPUT_CONFIRM_ACCEPTED
					   : CLI_INPUT_CONFIRM_DECLINED;
}
