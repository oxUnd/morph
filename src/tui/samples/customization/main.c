#include "application.h"
#include "pages/dashboard.h"

int main(void)
{
	struct sample_customization_application app = {0};
	const struct morph_tui_view_controller_spec root =
		sample_customization_page_dashboard_spec(&app);

	app.base.ops = sample_customization_application_methods();
	return morph_tui_application_main(&app.base, &root);
}
