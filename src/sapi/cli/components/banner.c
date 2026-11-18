#include "sapi/cli/components/banner.h"
#include "sapi/cli/adapters/media/asset.h"
#include "sapi/cli/components/report.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/banner.h"
#include "sapi/cli/presentation/output.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nothing/tui/views/image_view.h>

struct cli_banner {
	struct cli_report report;
	struct nt_view *logo;
};

static int cli_banner_format_report(struct cli_report *report, nb_buf *rendered, int width)
{
	struct cli_banner *banner = (struct cli_banner *)report;
	struct cli_application *ctx = cli_application_from_view(&report->base);
	return cli_banner_render(ctx, rendered, width, banner->logo != NULL);
}

static int cli_banner_layout(struct nt_view *view)
{
	struct cli_banner *banner = (struct cli_banner *)view;
	int rc = cli_report_class.layout(view);
	struct nt_view_rect rect = nt_view_content_rect(banner->report.text);
	if (!rc && banner->logo)
		rc = nt_view_set_hidden(banner->logo, rect.width < 30);

	if (!rc && banner->logo && !nt_view_is_hidden(banner->logo))
		rc = nt_view_arrange(banner->logo, (struct nt_view_rect){rect.x, rect.y, 2, 1});

	return rc;
}

static void cli_banner_asset_cleanup(struct nt_media_asset **owner)
{
	nt_media_asset_release(*owner);
	*owner = NULL;
}

static struct nt_view *cli_banner_logo_add_borrow(struct nt_builder *builder,
						  struct nt_view *parent,
						  struct cli_application *ctx)
{
	nb_buf bytes NB_CLEANUP(nb_buf_destroy) = {0};

	if (cli_banner_read_logo(ctx, &bytes) == NT_MEDIA_PROTOCOL_NONE)
		return NULL;

	struct nt_media_asset *asset NB_CLEANUP(cli_banner_asset_cleanup) = NULL;
	if (cli_media_asset_from_png(&asset, (const unsigned char *)nb_buf_cstr(&bytes), bytes.len))
		return NULL;

	const struct nt_view_options options = {.width = 2, .height = 1};
	return nt_image_view_add(builder, parent, asset, &options);
}

struct nt_view *cli_banner_add(struct nt_builder *builder, struct nt_view *parent)
{
	struct cli_application *ctx = cli_application_from_view(parent);
	if (!ctx) {
		nt_builder_fail(builder, NB_ERR_STATE);
		return NULL;
	}

	static const struct nt_view_class cli_banner_class = {
		.superclass = &cli_report_class,
		.instance_size = sizeof(struct cli_banner),
		.kind = "cli_banner",
		.layout = cli_banner_layout,
	};
	const struct nt_view_options options = {.width = NT_FILL, .margin = {.top = 1}};
	struct cli_banner *banner = (struct cli_banner *)cli_report_add(
		builder, parent, &cli_banner_class, &options, cli_output_color_enabled(ctx),
		cli_banner_format_report);
	if (!banner)
		return NULL;

	const struct nt_view_options gutters = {
		.padding = {.left = CLI_BANNER_PADDING, .right = CLI_BANNER_PADDING}};
	nt_builder_fail(builder, nt_view_set_options(banner->report.text, &gutters));
	banner->logo = cli_banner_logo_add_borrow(builder, &banner->report.base, ctx);
	return &banner->report.base;
}
