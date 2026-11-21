#include "sapi/cli/pages/setup/controls.h"

#include "sapi/cli/application.h"
#include "sapi/cli/components/setup_form.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/configuration/model.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nothing/tui/terminal.h>
#include <nothing/tui/views/stack_view.h>
#include <signal.h>

int cli_setup_controls_available(FILE *input, FILE *output)
{
	return nt_terminal_available(input, output);
}

struct cli_controls_setup_page {
	struct nt_view_controller base;
	struct cli_setup_state *state;
	const char *path;
	int styled;
};

static int cli_controls_setup_result(void *data, const struct nt_view_event *event)
{
	if (event->target != event->current)
		return NT_VIEW_EVENT_PASS;

	struct cli_controls_setup_page *page = data;
	int rc = nt_view_controller_dismiss(
		&page->base, event->type == NT_VIEW_EVENT_CANCEL ? NB_ERR_CANCELLED : NB_OK);
	return rc ? rc : NT_VIEW_EVENT_HANDLED;
}

static void cli_controls_setup_load(struct nt_view_controller *controller,
				    struct nt_builder *builder)
{
	struct cli_controls_setup_page *page = (struct cli_controls_setup_page *)controller;
	struct nt_view *root = nt_column_add(builder, NULL, 0, NULL);
	struct nt_view *form =
		cli_setup_form_add_borrow(builder, root, page->state, page->path, page->styled);
	if (!form)
		return;

	if (!nt_view_on(form, NT_VIEW_EVENT_SUBMIT, cli_controls_setup_result, page) ||
	    !nt_view_on(form, NT_VIEW_EVENT_CANCEL, cli_controls_setup_result, page))
		nt_builder_fail(builder, NB_ERR_NOMEM);
}

static int cli_controls_setup_event(struct nt_view_controller *controller,
				    const struct nt_event *event)
{
	if (event->type == NT_EVENT_EOF ||
	    (event->type == NT_EVENT_SIGNAL && (event->code == SIGINT || event->code == SIGTERM)) ||
	    (event->type == NT_EVENT_KEY && event->action != NT_KEY_ACTION_RELEASE &&
	     event->key == 27)) {
		int rc = nt_view_controller_dismiss(controller, NB_ERR_CANCELLED);
		return rc ? rc : 1;
	}

	return 0;
}

static const struct nt_view_controller_class cli_controls_setup_page_class = {
	.superclass = &cli_application_panel_class,
	.instance_size = sizeof(struct cli_controls_setup_page),
	.load_view = cli_controls_setup_load,
	.handle_event = cli_controls_setup_event,
};

int cli_setup_controls_collect(struct cli_application *ctx, const char *path, FILE *input,
			       FILE *output, struct cli_setup_state *state)
{
	cli_setup_role_defaults(state);
	if (!state->existing_prompt)
		state->replace_prompt = 1;

	struct nt_view_controller *owner NB_CLEANUP(cli_application_controller_cleanup) = NULL;
	int rc = nt_view_controller_create(&cli_controls_setup_page_class, &owner);
	if (rc)
		return rc;

	struct cli_controls_setup_page *page = (struct cli_controls_setup_page *)owner;
	page->state = state;
	page->path = path;
	page->styled = cli_output_color_enabled(ctx);
	return cli_application_run_page(ctx, &owner, input, output);
}
