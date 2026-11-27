#include "sapi/cli/components/activity.h"

#include "sapi/cli/components/report.h"
#include "sapi/cli/presentation/messages.h"
#include <nb/err.h>
#include <nb/cleanup.h>
#include <nothing/tui/views/text_view.h>
#include <stdlib.h>
#include <string.h>

struct cli_activity {
	struct nt_view base;
	struct nt_view *header, *compact;
	char *header_text, *compact_text;
	struct {
		struct nt_view *summary, *details;
		char *summary_text, *details_text;
	} groups[CLI_ACTIVITY_GROUP_COUNT];
	int styled;
};

static void cli_activity_deinit(struct nt_view *view)
{
	struct cli_activity *activity = (struct cli_activity *)view;
	free(activity->header_text);
	free(activity->compact_text);
	for (int group = 0; group < CLI_ACTIVITY_GROUP_COUNT; group++) {
		free(activity->groups[group].summary_text);
		free(activity->groups[group].details_text);
	}
}

static int cli_activity_text_set(struct nt_view *view, char **previous, const char *text,
	int styled, int shortcuts)
{
	if (!nt_view_is_owner_thread(view))
		return NB_ERR_NOT_PERMITTED;

	if (*previous && !strcmp(*previous, text))
		return 0;

	char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(text);
	if (!copy)
		return NB_ERR_NOMEM;

	int rc = shortcuts ? cli_messages_set_rendered_named_keys(view, text, styled)
		: cli_messages_set_rendered_view(view, text, styled);
	if (!rc) {
		free(*previous);
		*previous = nb_charp_steal(&copy);
	}

	return rc;
}

static int cli_activity_rows(struct nt_view *view, int width, int *rows)
{
	if (!*nt_text_view_text(view)) {
		*rows = 0;
		return 0;
	}

	struct nt_view_size size = {0};
	int rc = nt_view_measure(view, width, &size);
	*rows = size.height;
	return rc;
}

static int cli_activity_measure(struct nt_view *view, int width, struct nt_view_size *size)
{
	struct cli_activity *activity = (struct cli_activity *)view;
	if (!*nt_text_view_text(activity->header)) {
		*size = (struct nt_view_size){0};
		return 0;
	}

	int rows = 1;
	for (int group = 0; group < CLI_ACTIVITY_GROUP_COUNT; group++) {
		int details = 0;
		int rc = cli_activity_rows(activity->groups[group].details, width, &details);
		if (rc)
			return rc;

		rows += *nt_text_view_text(activity->groups[group].summary) ? 1 : 0;
		rows += details;
	}

	*size = (struct nt_view_size){rows ? width : 0, rows > 10 ? 10 : rows};
	return 0;
}

static int cli_activity_place(struct nt_view *view, struct nt_view_rect *slot, int rows)
{
	int height = rows < slot->height ? rows : slot->height;
	int rc = nt_view_arrange(view, (struct nt_view_rect){
		slot->x, slot->y, slot->width, height});
	slot->y += height;
	slot->height -= height;
	return rc;
}

static int cli_activity_layout(struct nt_view *view)
{
	struct cli_activity *activity = (struct cli_activity *)view;
	struct nt_view_rect slot = nt_view_content_rect(view);
	int rc = cli_activity_place(activity->header, &slot,
		*nt_text_view_text(activity->header) ? 1 : 0);
	int summaries = 0;
	for (int group = 0; group < CLI_ACTIVITY_GROUP_COUNT; group++)
		summaries += *nt_text_view_text(activity->groups[group].summary) ? 1 : 0;

	int compact = slot.height < summaries && *nt_text_view_text(activity->compact);
	if (!rc)
		rc = cli_activity_place(activity->compact, &slot, compact ? 1 : 0);

	int spare = slot.height > summaries ? slot.height - summaries : 0;
	for (int group = 0; !rc && group < CLI_ACTIVITY_GROUP_COUNT; group++) {
		int summary = !compact && *nt_text_view_text(activity->groups[group].summary);
		rc = cli_activity_place(activity->groups[group].summary, &slot, summary);
		int details = 0;
		if (!rc && !compact)
			rc = cli_activity_rows(activity->groups[group].details, slot.width, &details);

		if (details > spare)
			details = spare;

		if (!rc)
			rc = cli_activity_place(activity->groups[group].details, &slot, details);

		spare -= details;
	}

	return rc;
}

static const struct nt_view_class cli_activity_class = {
	.superclass = &nt_view_base_class,
	.instance_size = sizeof(struct cli_activity),
	.kind = "cli_activity",
	.measure = cli_activity_measure,
	.layout = cli_activity_layout,
	.deinit = cli_activity_deinit,
};

struct nt_view *cli_activity_add(struct nt_builder *builder, struct nt_view *parent, int styled)
{
	struct cli_activity *activity = (struct cli_activity *)nt_view_add(
		builder, parent, &cli_activity_class, NULL);
	if (!activity)
		return NULL;

	activity->styled = styled;
	struct nt_view *view = &activity->base;
	const struct nt_view_options row = {.height = 1};
	activity->header = cli_report_text_add(builder, view, &row);
	activity->compact = cli_report_text_add(builder, view, &row);
	for (int group = 0; group < CLI_ACTIVITY_GROUP_COUNT; group++) {
		activity->groups[group].summary = cli_report_text_add(builder, view, &row);
		activity->groups[group].details = cli_report_text_add(builder, view, NULL);
	}

	for (size_t index = 0; !nt_builder_error(builder) && index < nt_view_child_count(view);
	     index++)
		nt_builder_fail(builder, nt_text_view_set_wrap_mode(
			nt_view_child(view, index), NT_TEXT_WRAP_NONE));

	return nt_builder_error(builder) ? NULL : view;
}

int cli_activity_set_header(struct nt_view *view, const char *text)
{
	if (!view || view->type != &cli_activity_class || !text)
		return NB_ERR_INVAL;

	struct cli_activity *activity = (struct cli_activity *)view;
	return cli_activity_text_set(activity->header, &activity->header_text,
		text, activity->styled, 1);
}

int cli_activity_set_group(struct nt_view *view, enum cli_activity_group group,
	const char *summary, const char *details)
{
	if (!view || view->type != &cli_activity_class || group < 0 ||
	    group >= CLI_ACTIVITY_GROUP_COUNT || !summary || !details)
		return NB_ERR_INVAL;

	struct cli_activity *activity = (struct cli_activity *)view;
	int rc = cli_activity_text_set(activity->groups[group].summary,
		&activity->groups[group].summary_text, summary, activity->styled, 1);
	if (!rc)
		rc = cli_activity_text_set(activity->groups[group].details,
			&activity->groups[group].details_text, details, activity->styled, 0);

	return rc;
}

int cli_activity_set_compact(struct nt_view *view, const char *text)
{
	if (!view || view->type != &cli_activity_class || !text)
		return NB_ERR_INVAL;

	struct cli_activity *activity = (struct cli_activity *)view;
	return cli_activity_text_set(activity->compact, &activity->compact_text,
		text, activity->styled, 0);
}

struct nt_view *cli_activity_header_borrow(struct nt_view *view)
{
	return view && view->type == &cli_activity_class
		? ((struct cli_activity *)view)->header : NULL;
}

struct nt_view *cli_activity_details_borrow(struct nt_view *view, enum cli_activity_group group)
{
	return view && view->type == &cli_activity_class && group >= 0 &&
		group < CLI_ACTIVITY_GROUP_COUNT ? ((struct cli_activity *)view)->groups[group].details
		: NULL;
}
