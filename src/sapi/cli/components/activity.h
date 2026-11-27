#ifndef MORPH_CLI_COMPONENTS_ACTIVITY_H
#define MORPH_CLI_COMPONENTS_ACTIVITY_H

#include <nothing/tui/view.h>

enum cli_activity_group {
	CLI_ACTIVITY_AGENTS,
	CLI_ACTIVITY_JOBS,
	CLI_ACTIVITY_TASKS,
	CLI_ACTIVITY_QUEUE,
	CLI_ACTIVITY_AUXILIARY,
	CLI_ACTIVITY_GROUP_COUNT,
};

struct nt_view *cli_activity_add(struct nt_builder *, struct nt_view *parent, int styled);
int cli_activity_set_header(struct nt_view *, const char *text);
int cli_activity_set_group(struct nt_view *, enum cli_activity_group,
	const char *summary, const char *details);
int cli_activity_set_compact(struct nt_view *, const char *text);
struct nt_view *cli_activity_header_borrow(struct nt_view *);
struct nt_view *cli_activity_details_borrow(struct nt_view *, enum cli_activity_group);

#endif
