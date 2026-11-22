#include "sapi/cli/pages/confirmation.h"

#include "sapi/cli/pages/choice_dialog.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include <stdlib.h>
#include <string.h>

struct cli_confirmation {
	char *phrase;
	cli_confirmation_complete_fn complete;
	void *data;
	void (*release)(void *);
};

static void cli_confirmation_cleanup(struct cli_confirmation **owner)
{
	if (!*owner)
		return;

	free((*owner)->phrase);
	free(*owner);
	*owner = NULL;
}

static void cli_confirmation_release(void *data)
{
	struct cli_confirmation *confirmation NB_CLEANUP(cli_confirmation_cleanup) = data;
	if (confirmation->release)
		confirmation->release(confirmation->data);
}

static void cli_confirmation_complete(struct cli_application *ctx, int result, size_t index,
				      const char *answer, void *data)
{
	struct cli_confirmation *confirmation = data;
	int accepted =
		!result && (confirmation->phrase ? answer && !strcmp(answer, confirmation->phrase)
						 : index == 1);
	confirmation->complete(ctx, accepted, confirmation->data);
}

int cli_confirmation_show(struct cli_application *ctx, const char *phrase,
			  cli_confirmation_complete_fn complete, void *data,
			  void (*release)(void *))
{
	if (!complete)
		return NB_ERR_INVAL;

	struct cli_confirmation *confirmation NB_CLEANUP(cli_confirmation_cleanup) =
		calloc(1, sizeof(*confirmation));
	if (!confirmation)
		return NB_ERR_NOMEM;

	if (phrase) {
		confirmation->phrase = strdup(phrase);
		if (!confirmation->phrase)
			return NB_ERR_NOMEM;
	}

	confirmation->complete = complete;
	confirmation->data = data;
	confirmation->release = release;
	nb_buf question NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!(phrase ? nb_buf_printf(&question, "Type \"%s\" to confirm", phrase)
		     : nb_buf_append_cstr(&question, "Confirm this action?")))
		return nb_buf_status(&question);

	const char *choices[] = {"Cancel", "Confirm"};
	int rc = cli_choice_dialog_show(
		ctx, &(struct cli_choice_dialog_options){.question = nb_buf_cstr(&question),
							 .choices = choices,
							 .count = phrase ? 0 : 2,
							 .allow_custom = phrase != NULL,
							 .complete = cli_confirmation_complete,
							 .data = confirmation,
							 .release = cli_confirmation_release});
	if (!rc)
		confirmation = NULL;

	return rc;
}
