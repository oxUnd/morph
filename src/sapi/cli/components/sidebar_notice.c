#include "sapi/cli/components/sidebar_notice.h"

#include "sapi/cli/presentation/style.h"
#include "terminal/text.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/str.h>
#include <nothing/tui/theme.h>
#include <nothing/tui/views/label.h>
#include <string.h>

int cli_sidebar_notice_set_text(struct nt_view *view, const char *text)
{
	if (!view || !text)
		return NB_ERR_INVAL;

	char *safe NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(text,
		strlen(text), MORPH_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return NB_ERR_NOMEM;

	nb_str message = nb_str_trim(nb_str_from_cstr(safe));
	nb_buf formatted NB_CLEANUP(nb_buf_cleanup) = {0};
	if (message.len && (!nb_buf_append_cstr(&formatted, CLI_FEED_WARNING " ") ||
			    !nb_buf_append(&formatted, message.ptr, message.len)))
		return nb_buf_status(&formatted);

	int rc = nt_label_set_text(view, nb_buf_cstr(&formatted));
	return rc ? rc : nt_view_set_hidden(view, !message.len);
}

struct nt_view *cli_sidebar_notice_add(struct nt_builder *builder, struct nt_view *parent,
	const char *text, int styled)
{
	struct nt_view_options options = {.width = NT_FILL, .height = NT_AUTO,
		.padding = {.left = 1, .right = 1}};
	if (styled)
		options.style = nt_theme_style(nt_view_theme(parent), NT_THEME_WARNING,
			NT_VISUAL_NORMAL);

	struct nt_view *view = nt_label_add(builder, parent, "", &options);
	if (!view)
		return NULL;

	nt_builder_fail(builder, nt_view_set_selectable(view, 0));
	nt_builder_fail(builder, cli_sidebar_notice_set_text(view, text ? text : ""));
	return view;
}
