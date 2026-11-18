#include "cli_agents_browser_fixture.h"

#include <nb/cleanup.h>
#include <nothing/tui/application.h>
#include <nothing/tui/component_state.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/window.h>

struct browser_fixture {
	struct cli_agents_browser_state *state;
	const struct cli_agents_browser_data *data;
	struct nt_view *view;
	int styled;
};

static void browser_fixture_close(FILE **file)
{
	if (*file)
		fclose(*file);
}

static void browser_fixture_load(struct nt_view_controller *controller, struct nt_builder *builder)
{
	struct browser_fixture *fixture = controller->data;
	fixture->view = cli_agents_browser_add(builder, NULL);
	nt_builder_fail(builder, cli_agents_browser_sync(fixture->view, fixture->state,
							 fixture->data, fixture->styled));
}

int test_agents_browser_render(nb_buf *output, struct cli_agents_browser_state *state,
			       const struct cli_agents_browser_data *data, int columns, int rows,
			       int styled, test_agents_browser_refresh refresh)
{
	static const struct nt_view_controller_class type = {
		.superclass = &nt_view_controller_base_class,
		.instance_size = sizeof(struct nt_view_controller),
		.load_view = browser_fixture_load};
	struct browser_fixture fixture = {.state = state, .data = data, .styled = styled};
	struct nt_view_controller *controller = NULL;
	struct nt_surface *surface = NULL;
	FILE *input NB_CLEANUP(browser_fixture_close) = tmpfile();
	FILE *stream NB_CLEANUP(browser_fixture_close) = tmpfile();
	if (!input || !stream)
		return NB_ERR_IO;

	struct nt_application app = {0};
	struct nt_application_options options = {.input = input, .output = stream};
	int rc = nt_application_init(&app, &options);
	if (!rc)
		rc = nt_view_controller_create(&type, &controller);

	if (!rc) {
		controller->data = &fixture;
		rc = nt_application_present_view_controller(&app, &controller);
	}

	if (!rc)
		rc = nt_application_step(&app, 0);

	if (!rc && refresh) {
		rc = refresh(fixture.view, state, data, styled);
		if (!rc)
			rc = nt_view_flush_state(fixture.view);
	}

	if (!rc)
		rc = nt_surface_create(&surface);

	if (!rc) {
		struct nt_terminal_capabilities caps = *nt_surface_capabilities(surface);
		caps.color_profile = styled ? NT_COLOR_PROFILE_TRUECOLOR : NT_COLOR_PROFILE_NONE;
		nt_surface_set_capabilities(surface, &caps);
		rc = nt_surface_resize(surface, columns, rows);
	}

	if (!rc)
		rc = nt_view_arrange(fixture.view, (struct nt_view_rect){0, 0, columns, rows});

	if (!rc)
		rc = nt_view_render(fixture.view, surface);

	if (!rc)
		rc = nt_surface_rows(surface, output);

	nt_surface_destroy(surface);
	nt_view_controller_destroy(controller);
	nt_application_deinit(&app);
	return rc;
}
