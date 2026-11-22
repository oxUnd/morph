#include "sapi/cli/adapters/input/confirm.h"
#include <nb/err.h>
#include "sapi/cli/pages/chat.h"

#include <stdio.h>
#include <string.h>

#define CLI_CONFIRM_LINE_MAX 64

/* Returns 1 when a line was read, 0 without a controlling terminal, and a
   a negative status when the terminal could not be read. */
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
		return CLI_INPUT_CONFIRM_READ_ERROR;

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
		return CLI_INPUT_CONFIRM_READ_ERROR;

	rc = cli_confirm_read(answer, sizeof(answer));
	if (rc == 0)
		return CLI_INPUT_CONFIRM_NO_TERMINAL;

	if (rc < 0)
		return CLI_INPUT_CONFIRM_READ_ERROR;

	answer[strcspn(answer, "\r\n")] = '\0';
	return strcmp(answer, phrase) == 0 ? CLI_INPUT_CONFIRM_ACCEPTED
					   : CLI_INPUT_CONFIRM_DECLINED;
}
