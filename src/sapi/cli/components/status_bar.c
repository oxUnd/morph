#include "sapi/cli/components/status_bar.h"

#include "sapi/cli/presentation/style.h"
#include "terminal/text.h"
#include <nb/cleanup.h>
#include <nb/err.h>
#include <nb/str.h>
#include <nb/utf8.h>
#include <nothing/tui/controls/button.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>

#include <string.h>

struct cli_status_bar {
	struct nt_stack_view base;
	struct nt_view *prefix, *permission, *leading, *button, *caption, *trailing, *extra;
	int styled;
};

static const struct nt_view_class cli_status_bar_class = {
	.superclass = &nt_stack_view_class,
	.instance_size = sizeof(struct cli_status_bar),
	.kind = "cli_status_bar",
};

enum cli_status_bar_permission_tone {
	CLI_STATUS_PERMISSION_TONE_NONE = 0,
	CLI_STATUS_PERMISSION_TONE_ASK,
	CLI_STATUS_PERMISSION_TONE_AUTO,
	CLI_STATUS_PERMISSION_TONE_FULL,
	CLI_STATUS_PERMISSION_TONE_READ_ONLY,
};

static struct nt_text_style
cli_status_bar_permission_style(enum cli_status_bar_permission_tone tone)
{
	struct nt_text_style style = {0};
	switch (tone) {
	case CLI_STATUS_PERMISSION_TONE_FULL:
		style.attributes = NT_STYLE_BOLD;
		style.foreground =
			(struct nt_color){NT_COLOR_INDEXED, CLI_STATUS_PERM_FULL_FOREGROUND_INDEX};
		style.background =
			(struct nt_color){NT_COLOR_INDEXED, CLI_STATUS_PERM_FULL_BACKGROUND_INDEX};
		break;
	case CLI_STATUS_PERMISSION_TONE_ASK:
		style.foreground =
			(struct nt_color){NT_COLOR_INDEXED, CLI_STATUS_PERM_ASK_FOREGROUND_INDEX};
		style.background =
			(struct nt_color){NT_COLOR_INDEXED, CLI_STATUS_PERM_ASK_BACKGROUND_INDEX};
		break;
	case CLI_STATUS_PERMISSION_TONE_AUTO:
		style.foreground =
			(struct nt_color){NT_COLOR_INDEXED, CLI_STATUS_PERM_AUTO_FOREGROUND_INDEX};
		style.background =
			(struct nt_color){NT_COLOR_INDEXED, CLI_STATUS_PERM_AUTO_BACKGROUND_INDEX};
		break;
	case CLI_STATUS_PERMISSION_TONE_READ_ONLY:
		style.foreground =
			(struct nt_color){NT_COLOR_INDEXED,
					  CLI_STATUS_PERM_READ_ONLY_FOREGROUND_INDEX};
		style.background =
			(struct nt_color){NT_COLOR_INDEXED,
					  CLI_STATUS_PERM_READ_ONLY_BACKGROUND_INDEX};
		break;
	default:
		break;
	}

	return style;
}

/* Truncated footers end a preset name with a space or a UTF-8 ellipsis. */
static size_t cli_status_bar_permission_token_length(const char *text, size_t limit)
{
	size_t length = 0;
	while (length < limit && text[length] != ' ' && (unsigned char)text[length] != 0xE2)
		length++;

	return length;
}

static enum cli_status_bar_permission_tone cli_status_bar_permission_tone(const char *segment,
									  size_t length)
{
	static const struct {
		const char *name;
		enum cli_status_bar_permission_tone tone;
	} presets[] = {
		{"ask", CLI_STATUS_PERMISSION_TONE_ASK},
		{"auto", CLI_STATUS_PERMISSION_TONE_AUTO},
		{"full", CLI_STATUS_PERMISSION_TONE_FULL},
		{"read_only", CLI_STATUS_PERMISSION_TONE_READ_ONLY},
	};
	if (length <= 6 || strncmp(segment, "perm: ", 6))
		return CLI_STATUS_PERMISSION_TONE_NONE;

	const char *name = segment + 6;
	size_t name_length = cli_status_bar_permission_token_length(name, length - 6);
	for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
		size_t preset_length = strlen(presets[i].name);
		if (name_length == preset_length && !memcmp(name, presets[i].name, preset_length))
			return presets[i].tone;
	}

	return CLI_STATUS_PERMISSION_TONE_NONE;
}

static int cli_status_bar_permission_name_matches(const char *name)
{
	static const char *const names[] = {"ask", "auto", "full", "read_only", "custom", "?"};
	size_t length = cli_status_bar_permission_token_length(name, strlen(name));
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strlen(names[i]) == length && !memcmp(name, names[i], length))
			return 1;
	}

	return 0;
}

/* Locate a footer "perm: <preset>" segment after an identity separator. */
static const char *cli_status_bar_permission_find(const char *plain)
{
	const char *at = plain;
	while ((at = strstr(at, "perm: ")) != NULL) {
		int separated = at == plain || (at - plain >= 4 && !memcmp(at - 4, " · ", 4));
		if (separated && cli_status_bar_permission_name_matches(at + 6))
			return at;

		at += 6;
	}

	return NULL;
}

struct nt_view *cli_status_bar_add(struct nt_builder *builder, struct nt_view *parent, int styled)
{
	struct cli_status_bar *bar = (struct cli_status_bar *)nt_view_add(builder, parent,
		&cli_status_bar_class,
		&(struct nt_view_options){.padding = {.left = 2, .right = 2}});
	if (!bar)
		return NULL;

	struct nt_view *root = &bar->base.base;
	struct nt_view_options label = {0};
	if (styled)
		label.style.attributes = NT_STYLE_DIM;

	struct nt_view *row = nt_row_add(builder, root, 0, NULL);
	bar->prefix = nt_label_add(builder, row, "", &label);
	bar->permission = nt_label_add(builder, row, "", &(struct nt_view_options){.height = 1});
	bar->leading = nt_label_add(builder, row, "", &label);
	struct nt_view_options button = {.height = 1};
	if (styled) {
		button.style.foreground =
			(struct nt_color){NT_COLOR_INDEXED, CLI_STATUS_LINK_INDEX};
		button.style.attributes = NT_STYLE_UNDERLINE;
	}

	bar->button = nt_button_add(builder, row, "MCP", 0, &button);
	bar->caption = nt_label_add(builder, bar->button, "MCP",
		&(struct nt_view_options){.width = NT_FILL, .height = 1});
	bar->trailing = nt_label_add(builder, row, "", &label);
	bar->extra = nt_label_add(builder, root, "", &label);
	if (!nt_builder_error(builder)) {
		nt_builder_fail(builder, nt_label_set_opaque(bar->caption, 1));
		nt_builder_fail(builder, nt_view_set_selectable(bar->caption, 0));
		nt_builder_fail(builder, nt_label_set_opaque(bar->permission, styled));
		nt_builder_fail(builder, nt_view_set_selectable(bar->permission, 0));
		nt_builder_fail(builder, nt_view_set_hidden(bar->permission, 1));
		nt_builder_fail(builder, nt_view_set_hidden(bar->button, 1));
		nt_builder_fail(builder, nt_view_set_hidden(bar->extra, 1));
		nt_builder_fail(builder,
				nt_label_set_wrap_mode(bar->permission, NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, nt_label_set_wrap_mode(bar->prefix, NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, nt_label_set_wrap_mode(bar->leading, NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, nt_label_set_wrap_mode(bar->caption, NT_TEXT_WRAP_NONE));
		nt_builder_fail(builder, nt_label_set_wrap_mode(bar->trailing, NT_TEXT_WRAP_NONE));
	}

	bar->styled = styled;
	return nt_builder_error(builder) ? NULL : root;
}

int cli_status_bar_set_text(struct nt_view *view, const char *text)
{
	if (!view || view->type != &cli_status_bar_class || !text)
		return NB_ERR_INVAL;

	struct cli_status_bar *bar = (struct cli_status_bar *)view;
	char *plain NB_CLEANUP(nb_charp_cleanup) = morph_terminal_sanitize_dup(
		text, strlen(text), MORPH_TERMINAL_TEXT_MULTILINE, NULL);
	if (!plain)
		return NB_ERR_NOMEM;

	char *extra = strchr(plain, '\n');
	if (extra)
		*extra++ = '\0';

	char *mcp = strstr(plain, "MCP ");
	char *tail = mcp ? strstr(mcp, " · ") : NULL;
	const char *permission_start = bar->styled ? cli_status_bar_permission_find(plain) : NULL;
	const char *permission_end = NULL;
	size_t permission_length = 0;
	if (permission_start) {
		permission_end = strstr(permission_start + 6, " · ");
		permission_length =
			permission_end ? (size_t)(permission_end - permission_start)
				       : strlen(permission_start);
	}

	size_t prefix_length = permission_start ? (size_t)(permission_start - plain) : 0;
	if (permission_start && prefix_length > 0 && plain[prefix_length - 1] == ' ')
		prefix_length--;

	int has_permission = permission_start != NULL;
	nb_buf permission_text NB_CLEANUP(nb_buf_cleanup) = {0};
	if (has_permission &&
	    (!nb_buf_append_char(&permission_text, ' ') ||
	     !nb_buf_append(&permission_text, permission_start, permission_length) ||
	     !nb_buf_append_char(&permission_text, ' ')))
		return NB_ERR_NOMEM;

	size_t leading_start = 0;
	if (has_permission)
		leading_start = permission_end ? (size_t)(permission_end - plain) + 1
					       : strlen(plain);

	size_t leading_end = mcp ? (size_t)(mcp - plain) : strlen(plain);
	if (leading_end < leading_start)
		leading_end = leading_start;

	nb_buf prefix NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf leading NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf caption NB_CLEANUP(nb_buf_cleanup) = {0};
	if (!nb_buf_append(&prefix, plain, prefix_length) ||
	    !nb_buf_append(&leading, plain + leading_start, leading_end - leading_start) ||
	    (mcp && !nb_buf_append(&caption, mcp, tail ? (size_t)(tail - mcp) : strlen(mcp))))
		return NB_ERR_NOMEM;

	int rc = nt_label_set_text(bar->prefix, nb_buf_cstr(&prefix));
	if (!rc)
		rc = nt_view_set_hidden(bar->prefix, nb_buf_empty(&prefix));

	if (!rc)
		rc = nt_label_set_text(bar->permission, nb_buf_cstr(&permission_text));

	if (!rc)
		rc = nt_view_set_hidden(bar->permission, !has_permission);

	if (!rc) {
		enum cli_status_bar_permission_tone tone = CLI_STATUS_PERMISSION_TONE_NONE;
		struct nt_view_options options = nt_view_options(bar->permission);
		if (has_permission)
			tone = cli_status_bar_permission_tone(permission_start, permission_length);

		options.style = cli_status_bar_permission_style(tone);
		rc = nt_view_set_options(bar->permission, &options);
	}

	if (!rc)
		rc = nt_label_set_text(bar->leading, nb_buf_cstr(&leading));

	if (!rc)
		rc = nt_button_set_label(bar->button, nb_buf_cstr(&caption));

	if (!rc)
		rc = nt_label_set_text(bar->caption, nb_buf_cstr(&caption));

	struct nt_view_options options = nt_view_options(bar->button);
	options.width = (int)nb_utf8_width(nb_buf_str(&caption));
	if (!rc)
		rc = nt_view_set_options(bar->button, &options);

	if (!rc)
		rc = nt_view_set_hidden(bar->button, !mcp);

	if (!rc)
		rc = nt_label_set_text(bar->trailing, tail ? tail : "");

	if (!rc)
		rc = nt_label_set_text(bar->extra, extra ? extra : "");

	if (!rc)
		rc = nt_view_set_hidden(bar->extra, !extra || !*extra);

	return rc;
}
