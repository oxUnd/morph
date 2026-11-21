#include "sapi/cli/components/setup_prompt.h"

#include "sapi/cli/components/shortcut_hint.h"

#include "sapi/cli/services/configuration/role.h"
#include "sapi/cli/services/configuration/mcp.h"
#include <nb/cleanup.h>
#include <nothing/tui/controls/list_view.h>
#include <nothing/tui/terminal/text.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/views/card_view.h>
#include <nothing/tui/views/label.h>
#include <nothing/tui/views/stack_view.h>
#include <nothing/tui/views/text_view.h>

struct cli_setup_prompt_card {
	struct nt_view *box, *label, *description;
	struct nt_text_style edge, first, second;
};

struct cli_setup_prompt {
	struct nt_view_controller base;
	struct cli_setup_prompt_options state;
	struct nt_surface *surface;
	struct nt_view *progress, *scope, *title, *detail, *summary, *policy, *space;
	struct nt_view *list, *input, *cards, *preview[3], *gap, *hint;
	struct cli_setup_prompt_card *card;
	nb_buf row;
};

static struct nt_text_style cli_setup_prompt_style(int styled, unsigned attributes, int fg, int bg)
{
	struct nt_text_style style = {0};
	if (styled) {
		style.attributes = attributes;
		if (fg >= 0)
			style.foreground = (struct nt_color){
				fg < 16 ? NT_COLOR_ANSI : NT_COLOR_INDEXED, (unsigned)fg};

		if (bg >= 0)
			style.background = (struct nt_color){
				bg < 16 ? NT_COLOR_ANSI : NT_COLOR_INDEXED, (unsigned)bg};
	}

	return style;
}

static struct nt_view *cli_setup_prompt_label(struct nt_builder *builder, struct nt_view *parent,
					      const char *text, struct nt_text_style style)
{
	struct nt_view *label =
		nt_label_add(builder, parent, text ? text : "",
			     &(struct nt_view_options){.height = 1, .style = style});
	nt_builder_fail(builder, nt_label_set_wrap_mode(label, NT_TEXT_WRAP_NONE));
	return label;
}

static const char *cli_setup_prompt_item(void *data, size_t index)
{
	struct cli_setup_prompt *p = data;
	struct cli_setup_prompt_options *state = &p->state;
	const char *marker = index == state->menu->list.index ? "›" : " ";
	nb_buf_clear(&p->row);
	if (state->roles) {
		const char *tag = index != (size_t)state->current ? ""
				  : state->confirmed		  ? "  (current)"
								  : "  (default)";
		nb_buf_printf(&p->row, "%s %zu  %s%s", marker, index + 1, state->items[index], tag);
	} else
		nb_buf_printf(&p->row, "%s %s", marker, state->items[index]);

	return nb_buf_cstr(&p->row);
}

static int cli_setup_prompt_draw_row(void *data, struct nt_canvas *canvas, size_t index,
				     size_t group, int selected)
{
	struct cli_setup_prompt *p = data;
	(void)group;
	const char *label = cli_setup_prompt_item(p, index);
	char *clean NB_CLEANUP(nb_charp_cleanup) =
		nt_sanitize_dup(label, strlen(label), NT_TEXT_SINGLE_LINE, NULL);
	if (!clean)
		return NB_ERR_NOMEM;

	struct nt_text_style style =
		cli_setup_prompt_style(p->state.styled && selected, NT_STYLE_BOLD, 0, 6);
	return nt_canvas_text_styled(canvas, 0, 0, clean, &style);
}

static struct nt_text_style cli_setup_prompt_card_style(void *data, int row, int height)
{
	const struct cli_setup_prompt_card *card = data;
	return row == 0 || row == height - 1 ? card->edge : row == 1 ? card->first : card->second;
}

static void cli_setup_prompt_build(struct nt_view_controller *controller,
				   struct nt_builder *builder)
{
	struct cli_setup_prompt *p = (struct cli_setup_prompt *)controller;
	const struct cli_setup_prompt_options *s = &p->state;
	struct nt_text_style dim = cli_setup_prompt_style(s->styled, NT_STYLE_DIM, -1, -1);
	struct nt_text_style cyan = cli_setup_prompt_style(s->styled, 0, 6, -1);
	struct nt_text_style bold = cli_setup_prompt_style(s->styled, NT_STYLE_BOLD, -1, -1);
	struct nt_view *root = nt_column_add(
		builder, NULL, 0, &(struct nt_view_options){.padding = {.left = 2, .right = 2}});
	p->progress = cli_setup_prompt_label(builder, root, s->progress, dim);
	p->scope = cli_setup_prompt_label(builder, root, s->scope, cyan);
	p->title = cli_setup_prompt_label(builder, root, s->title, bold);
	p->detail = cli_setup_prompt_label(builder, root, s->detail, dim);
	p->summary = cli_setup_prompt_label(builder, root, s->summary, dim);
	p->policy = cli_setup_prompt_label(builder, root,
					   "Read/media: auto-approved. Host exec: on.", dim);
	p->space = cli_setup_prompt_label(builder, root, "", dim);
	if (s->editor)
		p->input = cli_setup_prompt_label(builder, root, "", cyan);
	else {
		p->list = nt_list_view_add(builder, root, NULL, 0, 0, NULL);
		nt_builder_fail(builder, nt_list_view_set_source(p->list, (size_t)s->count,
								 cli_setup_prompt_item, p));
		nt_builder_fail(builder, nt_list_view_set_row_renderer(
						 p->list, cli_setup_prompt_draw_row, p));
		if (p->card) {
			p->cards = nt_column_add(builder, root, 0, NULL);
			for (int i = 0; i < s->count; i++) {
				struct cli_setup_prompt_card *card = &p->card[i];
				card->box = nt_block_view_add(
					builder, p->cards,
					&(struct nt_view_options){.height = 4, .max_width = 72});
				nt_builder_fail(
					builder,
					nt_card_view_set_row_style(
						card->box, cli_setup_prompt_card_style, card));
				struct nt_view *body =
					nt_column_add(builder, card->box, 0,
						      &(struct nt_view_options){
							      .padding = {.left = 1, .right = 1}});
				card->label = cli_setup_prompt_label(builder, body, "", bold);
				card->description = cli_setup_prompt_label(builder, body,
									   s->descriptions[i], dim);
			}
		}

		if (s->roles) {
			p->gap = cli_setup_prompt_label(builder, root, "", dim);
			for (size_t i = 0; i < 3; i++) {
				p->preview[i] = nt_text_view_add(
					builder, root, "", &(struct nt_view_options){.style = dim});
				nt_builder_fail(builder, nt_text_view_set_wrap_mode(
								 p->preview[i], NT_TEXT_WRAP_CHAR));
			}
		}
	}

	p->hint = cli_shortcut_hint_add(builder, root, "",
		&(struct nt_view_options){.height = 1});
}

static void cli_setup_prompt_deinit(struct nt_view_controller *controller)
{
	struct cli_setup_prompt *p = (struct cli_setup_prompt *)controller;
	nt_surface_destroy(p->surface);
	nb_buf_cleanup(&p->row);
	free(p->card);
}

int cli_setup_prompt_create(const struct cli_setup_prompt_options *state,
			    struct cli_setup_prompt **out)
{
	if (!state || !out || *out)
		return NB_ERR_INVAL;

	static const struct nt_view_controller_class type = {
		.superclass = &nt_view_controller_base_class,
		.instance_size = sizeof(struct cli_setup_prompt),
		.load_view = cli_setup_prompt_build,
		.deinit = cli_setup_prompt_deinit,
	};
	struct nt_view_controller *controller = NULL;
	int rc = nt_view_controller_create(&type, &controller);
	if (rc)
		return rc;

	struct cli_setup_prompt *p NB_CLEANUP(cli_setup_prompt_cleanup) =
		(struct cli_setup_prompt *)controller;
	p->state = *state;
	if (state->descriptions && !state->roles) {
		p->card = calloc((size_t)state->count, sizeof(*p->card));
		if (!p->card)
			return NB_ERR_NOMEM;
	}

	rc = nt_surface_create(&p->surface);
	if (!rc)
		rc = nt_view_controller_load(&p->base);

	if (!rc) {
		*out = p;
		p = NULL;
	}

	return rc;
}

void cli_setup_prompt_cleanup(struct cli_setup_prompt **owner)
{
	if (*owner) {
		nt_view_controller_destroy(&(*owner)->base);
		*owner = NULL;
	}
}

static int cli_setup_prompt_height(struct nt_view *view, int rows)
{
	struct nt_view_options o = nt_view_options(view);
	int rc = nt_view_set_hidden(view, rows < 1);
	if (!rc && rows > 0 && o.height != rows) {
		o.height = rows;
		rc = nt_view_set_options(view, &o);
	}

	return rc;
}

static int cli_setup_prompt_preview(struct cli_setup_prompt *p, int columns, int rows,
				    int *available)
{
	const char *names[] = {"Text", "Vision", "Image", "Video"};
	nb_buf models NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf_append_cstr(&models, "Models: ");
	int role = (int)p->state.menu->list.index, used = 0;
	for (int i = 0; i < CLI_SETUP_MODEL_COUNT; i++)
		if (cli_setup_role_requires(role, i))
			nb_buf_printf(&models, "%s%s", used++ ? " + " : "", names[i]);

	if (!nb_buf_ok(&models))
		return NB_ERR_NOMEM;

	const char *lines[] = {cli_setup_role_get(role).description, nb_buf_cstr(&models),
			       cli_setup_mcp_summary(role)};
	const int priority[] = {1, 0, 2};
	int budget = *available > 1 ? *available - 1 : 0, total = 0;
	for (int n = 0; n < 3; n++) {
		int i = priority[n], visible = 0;
		if ((i != 0 || rows >= 14) && (i != 2 || rows >= 20)) {
			struct nt_text_layout wrap NB_CLEANUP(nt_text_layout_deinit) = {0};
			nt_text_layout_init(&wrap);
			int rc = nt_text_layout_build(&wrap, lines[i], (size_t)columns,
						      NT_TEXT_WRAP_CHAR);
			if (rc)
				return rc;

			if (wrap.lines.len <= (size_t)budget)
				visible = (int)wrap.lines.len;
			else if (i == 1)
				visible = budget;
		}

		int rc = nt_text_view_set_text(p->preview[i], lines[i]);
		if (!rc)
			rc = cli_setup_prompt_height(p->preview[i], visible);

		if (rc)
			return rc;

		budget -= visible;
		total += visible;
	}

	int gap = rows >= 20 && budget > 0 && total > 0;
	*available -= total + gap;
	return nt_view_set_hidden(p->gap, !gap);
}

static int cli_setup_prompt_choices(struct cli_setup_prompt *p, int width, int rows, int available)
{
	const struct cli_setup_prompt_options *s = &p->state;
	int rc = s->roles ? cli_setup_prompt_preview(p, width, rows, &available) : 0;
	int cards = p->card && width >= 40 && available >= 8;
	int visible = cards ? available / 4 : available;
	if (visible < 1)
		visible = 1;

	if (visible > s->count)
		visible = s->count;

	nt_list_state_layout(&s->menu->list, (size_t)s->count, (size_t)visible);
	if (!rc)
		rc = cli_setup_prompt_height(p->list, cards ? 0 : visible);

	if (!rc)
		rc = nt_list_view_select(p->list, s->menu->list.index);

	if (!rc && p->cards)
		rc = nt_view_set_hidden(p->cards, !cards);

	for (int i = 0; !rc && cards && i < s->count; i++) {
		struct cli_setup_prompt_card *card = &p->card[i];
		int selected = (size_t)i == s->menu->list.index;
		int fg = selected ? 6 : -1, bg = selected ? 235 : -1;
		struct nt_text_style style =
			cli_setup_prompt_style(s->styled, selected ? NT_STYLE_BOLD : 0, fg, bg);
		rc = nt_view_set_hidden(card->box,
					(size_t)i < s->menu->list.first ||
						(size_t)i >= s->menu->list.first + (size_t)visible);
		card->edge = cli_setup_prompt_style(s->styled, selected ? 0 : NT_STYLE_DIM, fg, -1);
		card->first = style;
		card->second = cli_setup_prompt_style(s->styled, NT_STYLE_DIM, fg, bg);
		if (!rc)
			rc = nt_label_set_text(card->label, cli_setup_prompt_item(p, (size_t)i));

		if (!rc)
			rc = nt_view_set_style(card->label, style);

		if (!rc)
			rc = nt_view_set_style(
				card->description,
				cli_setup_prompt_style(s->styled, NT_STYLE_DIM, fg, bg));
	}

	const char *hint = s->roles	? (width < 46 ? "↑↓ Move  Enter OK  Esc"
						      : "↑↓ Move · 1–8 Jump · Enter Select · Esc Back")
			   : width < 56 ? "↑↓ Move  Enter OK  Esc Back"
					: "↑↓ Choose · Enter Select · Esc Back · Ctrl+C Exit";
	if (!rc)
		rc = cli_shortcut_hint_set_text(p->hint, hint);

	if (!rc)
		rc = nt_view_set_hidden(p->hint, rows < 4);

	return rc;
}

int cli_setup_prompt_present(struct cli_setup_prompt *p, struct nt_inline_region *region,
			     FILE *output, int columns, int rows, const char *validation)
{
	struct cli_setup_prompt_options *s = &p->state;
	struct nt_view *root = nt_view_controller_root_view(&p->base);
	int scope = s->scope && *s->scope;
	const char *detail = s->editor && s->editor->overflow
				     ? "Value is too long. Shorten it before continuing."
			     : validation ? validation
					  : s->detail;
	int has_detail = rows >= 14 && detail;
	int summary = s->summary && rows >= 16;
	struct nt_view *views[] = {p->progress, p->scope,  p->detail,
				   p->summary,	p->policy, p->space};
	int visible[] = {rows >= 8, scope, has_detail, summary, summary, rows >= 20};
	int header = 1;
	int rc = nt_label_set_text(p->title, validation && rows < 14 ? "Value required" : s->title);
	for (size_t i = 0; !rc && i < sizeof(views) / sizeof(views[0]); i++) {
		rc = nt_view_set_hidden(views[i], !visible[i]);
		header += visible[i];
	}

	if (!rc)
		rc = nt_label_set_text(p->detail, detail ? detail : "");

	int width = columns > 4 ? columns - 4 : 1;
	int x = -1, y = -1;
	if (!rc && s->editor) {
		nb_buf input NB_CLEANUP(nb_buf_cleanup) = {0};
		size_t caret = 0;
		rc = nt_text_editor_prompt_line(s->editor, &input, (size_t)width, &caret);
		if (!rc)
			rc = nt_label_set_text(p->input, nb_buf_cstr(&input));

		if (!rc)
			rc = cli_shortcut_hint_set_text(
				p->hint,
				columns < 60
					? "Enter OK  Esc Back  Ctrl+U Clear"
					: "Enter Continue · Esc Back · Ctrl+U Clear · Ctrl+C Exit");

		x = (int)caret + 2;
		y = header;
	} else if (!rc)
		rc = cli_setup_prompt_choices(p, width, rows, rows - header - 2);

	struct nt_view_size size = {0};
	if (!rc)
		rc = nt_view_measure(root, columns, &size);

	if (!rc)
		rc = nt_surface_resize(p->surface, columns, size.height);

	if (!rc) {
		nt_surface_clear(p->surface);
		rc = nt_view_arrange(root, (struct nt_view_rect){0, 0, columns, size.height});
	}

	if (!rc)
		rc = nt_view_render(root, p->surface);

	if (!rc)
		rc = nt_inline_region_present(region, output, p->surface, columns, rows, x, y,
					      s->styled);

	return rc;
}
