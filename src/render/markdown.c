#include "markdown.h"
#include "highlight.h"
#include "util/array.h"
#include "util/buf.h"
#include "util/utf8.h"
#include "mathjax.h"
#include "md4c.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <limits.h>

#define ANSI_RESET     "\033[0m"
#define ANSI_BOLD      "\033[1m"
#define ANSI_DIM       "\033[2m"
#define ANSI_ITALIC    "\033[3m"
#define ANSI_UNDERLINE "\033[4m"
#define ANSI_STRIKE    "\033[9m"
#define ANSI_RED       "\033[31m"
#define ANSI_GREEN     "\033[32m"
#define ANSI_YELLOW    "\033[33m"
#define ANSI_BLUE      "\033[34m"
#define ANSI_MAGENTA   "\033[35m"
#define ANSI_CYAN      "\033[36m"
#define ANSI_GRAY      "\033[90m"

#define MATHJAX_DPI 72
#define MATHJAX_FG_COLOR 0x00FFFFFFu
#define MATHJAX_BG_COLOR 0x00000000u
#define MATHJAX_FALLBACK_CELL_WIDTH_PX 8u
#define MATHJAX_FALLBACK_CELL_HEIGHT_PX 18u
#define MATHJAX_INLINE_DEFAULT_SCALE 1.08
#define MATHJAX_INLINE_MIN_SIZE 15.0
#define MATHJAX_DISPLAY_DEFAULT_SCALE 1.60
#define MATHJAX_DISPLAY_MIN_SIZE 24.0
#define KITTY_PLACEHOLDER_CP 0x10EEEEu
#define KITTY_MAX_DIACRITIC_NUM 296u
#define FORMULA_MARKER "\x1fMF:"
#define FORMULA_MARKER_LEN 4u

/* ---------------- text buffer ---------------- */
/* sbuf is defined in highlight.h */

static const unsigned int kitty_num_to_diacritic[] = {
	0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F,
	0x0346, 0x034A, 0x034B, 0x034C, 0x0350, 0x0351, 0x0352, 0x0357,
	0x035B, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369,
	0x036A, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F, 0x0483, 0x0484,
	0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597,
	0x0598, 0x0599, 0x059C, 0x059D, 0x059E, 0x059F, 0x05A0, 0x05A1,
	0x05A8, 0x05A9, 0x05AB, 0x05AC, 0x05AF, 0x05C4, 0x0610, 0x0611,
	0x0612, 0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657, 0x0658,
	0x0659, 0x065A, 0x065B, 0x065D, 0x065E, 0x06D6, 0x06D7, 0x06D8,
	0x06D9, 0x06DA, 0x06DB, 0x06DC, 0x06DF, 0x06E0, 0x06E1, 0x06E2,
	0x06E4, 0x06E7, 0x06E8, 0x06EB, 0x06EC, 0x0730, 0x0732, 0x0733,
	0x0735, 0x0736, 0x073A, 0x073D, 0x073F, 0x0740, 0x0741, 0x0743,
	0x0745, 0x0747, 0x0749, 0x074A, 0x07EB, 0x07EC, 0x07ED, 0x07EE,
	0x07EF, 0x07F0, 0x07F1, 0x07F3, 0x0816, 0x0817, 0x0818, 0x0819,
	0x081B, 0x081C, 0x081D, 0x081E, 0x081F, 0x0820, 0x0821, 0x0822,
	0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082A, 0x082B, 0x082C,
	0x082D, 0x0951, 0x0953, 0x0954, 0x0F82, 0x0F83, 0x0F86, 0x0F87,
	0x135D, 0x135E, 0x135F, 0x17DD, 0x193A, 0x1A17, 0x1A75, 0x1A76,
	0x1A77, 0x1A78, 0x1A79, 0x1A7A, 0x1A7B, 0x1A7C, 0x1B6B, 0x1B6D,
	0x1B6E, 0x1B6F, 0x1B70, 0x1B71, 0x1B72, 0x1B73, 0x1CD0, 0x1CD1,
	0x1CD2, 0x1CDA, 0x1CDB, 0x1CE0, 0x1DC0, 0x1DC1, 0x1DC3, 0x1DC4,
	0x1DC5, 0x1DC6, 0x1DC7, 0x1DC8, 0x1DC9, 0x1DCB, 0x1DCC, 0x1DD1,
	0x1DD2, 0x1DD3, 0x1DD4, 0x1DD5, 0x1DD6, 0x1DD7, 0x1DD8, 0x1DD9,
	0x1DDA, 0x1DDB, 0x1DDC, 0x1DDD, 0x1DDE, 0x1DDF, 0x1DE0, 0x1DE1,
	0x1DE2, 0x1DE3, 0x1DE4, 0x1DE5, 0x1DE6, 0x1DFE, 0x20D0, 0x20D1,
	0x20D4, 0x20D5, 0x20D6, 0x20D7, 0x20DB, 0x20DC, 0x20E1, 0x20E7,
	0x20E9, 0x20F0, 0x2CEF, 0x2CF0, 0x2CF1, 0x2DE0, 0x2DE1, 0x2DE2,
	0x2DE3, 0x2DE4, 0x2DE5, 0x2DE6, 0x2DE7, 0x2DE8, 0x2DE9, 0x2DEA,
	0x2DEB, 0x2DEC, 0x2DED, 0x2DEE, 0x2DEF, 0x2DF0, 0x2DF1, 0x2DF2,
	0x2DF3, 0x2DF4, 0x2DF5, 0x2DF6, 0x2DF7, 0x2DF8, 0x2DF9, 0x2DFA,
	0x2DFB, 0x2DFC, 0x2DFD, 0x2DFE, 0x2DFF, 0xA66F, 0xA67C, 0xA67D,
	0xA6F0, 0xA6F1, 0xA8E0, 0xA8E1, 0xA8E2, 0xA8E3, 0xA8E4, 0xA8E5,
	0xA8E6, 0xA8E7, 0xA8E8, 0xA8E9, 0xA8EA, 0xA8EB, 0xA8EC, 0xA8ED,
	0xA8EE, 0xA8EF, 0xA8F0, 0xA8F1, 0xAAB0, 0xAAB2, 0xAAB3, 0xAAB7,
	0xAAB8, 0xAABE, 0xAABF, 0xAAC1, 0xFE20, 0xFE21, 0xFE22, 0xFE23,
	0xFE24, 0xFE25, 0xFE26, 0x10A0F, 0x10A38, 0x1D185, 0x1D186,
	0x1D187, 0x1D188, 0x1D189, 0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD,
	0x1D242, 0x1D243, 0x1D244,
};

/* ---------------- table buffering ---------------- */
struct table_cell {
	char *plain;	/* visible text, ANSI stripped, for width calc */
	size_t plain_len;
	char *raw;	/* full ANSI-decorated text */
	size_t raw_len;
};

struct table_row {
	struct table_cell *cells;
	unsigned cell_count;
};

struct table_state {
	unsigned col_count;
	MD_ALIGN *align;
	morph_array_t rows;
	int in_head;
	unsigned head_row_count;
	/* current cell being built */
	morph_buf_t cell_raw;
	int cell_active;
};

/* ---------------- terminal width ---------------- */
static int get_term_width(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return (int)ws.ws_col;
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return (int)ws.ws_col;
	const char *cols = getenv("COLUMNS");
	if (cols) {
		int v = atoi(cols);
		if (v > 0)
			return v;
	}
	return 80;
}

static void get_term_cell_size(unsigned int *out_w, unsigned int *out_h)
{
        struct winsize ws;
        unsigned int cell_w = MATHJAX_FALLBACK_CELL_WIDTH_PX;
        unsigned int cell_h = MATHJAX_FALLBACK_CELL_HEIGHT_PX;

        memset(&ws, 0, sizeof(ws));
        if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 ||
             ws.ws_col == 0 || ws.ws_row == 0 ||
             ws.ws_xpixel == 0 || ws.ws_ypixel == 0) &&
            (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0 ||
             ws.ws_col == 0 || ws.ws_row == 0 ||
             ws.ws_xpixel == 0 || ws.ws_ypixel == 0)) {
                if (out_w)
                        *out_w = cell_w;
                if (out_h)
                        *out_h = cell_h;
                return;
        }

        cell_w = ((unsigned int)ws.ws_xpixel + ws.ws_col - 1u) / ws.ws_col;
        cell_h = ((unsigned int)ws.ws_ypixel + ws.ws_row - 1u) / ws.ws_row;

        if (out_w)
                *out_w = cell_w > 0 ? cell_w : MATHJAX_FALLBACK_CELL_WIDTH_PX;
        if (out_h)
                *out_h = cell_h > 0 ? cell_h : MATHJAX_FALLBACK_CELL_HEIGHT_PX;
}

static int env_truthy(const char *v)
{
	if (!v || !*v)
		return 0;
	return strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
	       strcmp(v, "TRUE") == 0 || strcmp(v, "yes") == 0 ||
	       strcmp(v, "YES") == 0 || strcmp(v, "on") == 0 ||
	       strcmp(v, "ON") == 0;
}

static int env_falsey(const char *v)
{
	if (!v || !*v)
		return 0;
	return strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
	       strcmp(v, "FALSE") == 0 || strcmp(v, "no") == 0 ||
	       strcmp(v, "NO") == 0 || strcmp(v, "off") == 0 ||
	       strcmp(v, "OFF") == 0;
}

static double env_double_or(const char *name, double fallback)
{
	const char *v = getenv(name);
	char *end = NULL;
	double d;

	if (!v || !*v)
		return fallback;

	d = strtod(v, &end);
	if (end == v || d <= 0.0 || d > 8.0)
		return fallback;
	return d;
}

static double env_positive_double_or_zero(const char *name)
{
	const char *v = getenv(name);
	char *end = NULL;
	double d;

	if (!v || !*v)
		return 0.0;

	d = strtod(v, &end);
	if (end == v || d <= 0.0)
		return 0.0;
	return d;
}

static double mathjax_font_size_from_cell(unsigned int cell_h,
					  double default_scale,
					  double min_size,
					  const char *scale_env,
					  const char *max_env)
{
	double scale = env_double_or(scale_env, default_scale);
	double size = (double)cell_h * scale;
	double max_size = env_positive_double_or_zero(max_env);

	if (size < min_size)
		size = min_size;
	if (max_size > 0.0 && size > max_size)
		size = max_size;
	return size;
}

static int detect_kitty_placeholders(void)
{
	const char *force = getenv("MORPH_MARKDOWN_KITTY_PLACEHOLDER");
	const char *term;

	if (env_truthy(force))
		return 1;
	if (env_falsey(force))
		return 0;
	if (getenv("KITTY_WINDOW_ID"))
		return 1;
	term = getenv("TERM");
	return term && strstr(term, "kitty");
}

/* ---------------- line wrapping ---------------- */
struct wrapped_line {
	char *raw;	 /* ANSI-decorated text for this sub-line */
	size_t raw_len;
	size_t vis_width; /* visible column width (ANSI stripped) */
};

/* ---------------- main context ---------------- */
struct collected_media {
	char *type;
	char *path;
};

struct formula_atom {
	unsigned int id;
	char *transfer;
	size_t transfer_len;
	unsigned int cols;
	unsigned int rows;
	unsigned int image_id;
	int use_placeholder;
	int emitted;
};

struct ansi_ctx {
	struct sbuf out;	/* primary output */
	int bold_depth;
	int italic_depth;
	int code_depth;
	int strike_depth;
	int underline_depth;
	int link_depth;
	int quote_depth;
	int list_depth;
	int olist_index[16];	/* counter per nested ordered list */
	int list_is_ol[16];	/* 1 = ordered, 0 = unordered */
	int li_first_para[16];	/* per list level: is the next paragraph the first item content? */
	int in_heading;
	int last_block_was_visible;
	struct table_state *table;
	int current_link_href_pending;
	int term_width;
	struct sbuf link_href;
	char code_lang[32];
	size_t code_lang_len;
	morph_buf_t code_raw;
	int in_code_block;
	int latex_display;
	int use_kitty_placeholders;
	unsigned int next_image_id;
	unsigned int next_formula_id;
	morph_array_t formulas;
	int formulas_init;
	int inline_active;
	morph_buf_t inline_raw;
	struct collected_media media[64];
	int media_count;
};

/* Append to either the active table cell or the main output, depending on
 * whether we are currently rendering a table cell. */
static void out_append_n(struct ansi_ctx *ctx, const char *src, size_t n)
{
	if (ctx->table && ctx->table->cell_active) {
		morph_buf_append(&ctx->table->cell_raw, src, n);
		return;
	}
	if (ctx->inline_active) {
		morph_buf_append(&ctx->inline_raw, src, n);
		return;
	}
	sbuf_append_n(&ctx->out, src, n);
}

static void out_append(struct ansi_ctx *ctx, const char *str)
{
	out_append_n(ctx, str, strlen(str));
}

static struct table_row *table_rows(struct table_state *t)
{
	if (!t)
		return NULL;
	return (struct table_row *)t->rows.elts;
}

static void render_inline_layout(struct ansi_ctx *ctx, const char *raw,
				 size_t raw_len, size_t max_cols,
				 int to_table_cell, morph_buf_t *cell_out);
static int inline_raw_has_formula(const char *raw, size_t raw_len);
static size_t inline_raw_natural_width(struct ansi_ctx *ctx, const char *raw,
				       size_t raw_len);
static void append_utf8_cp(struct ansi_ctx *ctx, unsigned int cp);
static int render_mathjax_kitty(struct ansi_ctx *ctx, const char *latex,
				size_t len, int display);

/* Re-apply currently-active inline styles after an ANSI_RESET. Used when we
 * leave a span but still are inside other spans (e.g. **bold _italic_** when
 * the inner italic ends, we must restore bold). */
static void reapply_inline(struct ansi_ctx *ctx)
{
	if (ctx->bold_depth > 0)
		out_append(ctx, ANSI_BOLD);
	if (ctx->italic_depth > 0)
		out_append(ctx, ANSI_ITALIC);
	if (ctx->underline_depth > 0)
		out_append(ctx, ANSI_UNDERLINE);
	if (ctx->strike_depth > 0)
		out_append(ctx, ANSI_STRIKE);
}

/* Emit an indentation prefix for nested list / quote content. Called at the
 * start of every new logical line. */
static void emit_line_prefix(struct ansi_ctx *ctx)
{
	for (int i = 0; i < ctx->quote_depth; i++) {
		out_append(ctx, ANSI_DIM);
		out_append(ctx, "│ ");
		out_append(ctx, ANSI_RESET);
		reapply_inline(ctx);
	}
}

/* Emit a newline followed by the active line prefix. */
static void newline_with_prefix(struct ansi_ctx *ctx)
{
	out_append(ctx, "\n");
	emit_line_prefix(ctx);
}

/* Ensure we are at the start of a new line; insert a blank line if there was
 * any visible content before. Inside a blockquote the blank line does not get
 * the "│ " prefix — only content lines do. */
static void ensure_blank_line(struct ansi_ctx *ctx)
{
	if (ctx->last_block_was_visible) {
		out_append(ctx, "\n");
		if (ctx->quote_depth > 0) {
			out_append(ctx, "\n");
		} else {
			out_append(ctx, "\n");
		}
	}
}

/* Ensure the cursor is at the start of a content line with the correct
 * quote/list prefix. If we are not already at the start of a fresh line
 * (i.e. last_block_was_visible), emit a newline + prefix. Otherwise just
 * emit the prefix for the current line. */
static void start_content_line(struct ansi_ctx *ctx)
{
	if (ctx->last_block_was_visible)
		newline_with_prefix(ctx);
	else
		emit_line_prefix(ctx);
}

/* ---------------- block handlers ---------------- */
static int enter_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
	struct ansi_ctx *ctx = userdata;

	switch (type) {
	case MD_BLOCK_DOC:
		break;
	case MD_BLOCK_H: {
		struct MD_BLOCK_H_DETAIL *h = detail;
		ensure_blank_line(ctx);
		ctx->in_heading = 1;
		out_append(ctx, ANSI_BOLD);
		switch (h->level) {
		case 1: out_append(ctx, ANSI_CYAN); break;
		case 2: out_append(ctx, ANSI_GREEN); break;
		case 3: out_append(ctx, ANSI_YELLOW); break;
		case 4: out_append(ctx, ANSI_MAGENTA); break;
		case 5: out_append(ctx, ANSI_BLUE); break;
		default: out_append(ctx, ANSI_GRAY); break;
		}
		for (unsigned i = 0; i < h->level; i++)
			out_append(ctx, "#");
		out_append(ctx, " ");
		break;
	}
	case MD_BLOCK_CODE: {
		struct MD_BLOCK_CODE_DETAIL *c = detail;
		ensure_blank_line(ctx);
		ctx->code_depth++;
		ctx->in_code_block = 1;
		morph_buf_cleanup(&ctx->code_raw);
		morph_buf_init(&ctx->code_raw, 256);
		if (c->lang.text && c->lang.size > 0) {
			size_t n = c->lang.size < 31 ? c->lang.size : 31;
			memcpy(ctx->code_lang, c->lang.text, n);
			ctx->code_lang[n] = '\0';
			ctx->code_lang_len = n;
		} else {
			ctx->code_lang[0] = '\0';
			ctx->code_lang_len = 0;
		}
		out_append(ctx, ANSI_DIM);
		out_append(ctx, "```");
		if (ctx->code_lang_len > 0)
			out_append_n(ctx, ctx->code_lang, ctx->code_lang_len);
		out_append(ctx, ANSI_RESET);
		newline_with_prefix(ctx);
		break;
	}
	case MD_BLOCK_HTML:
		ensure_blank_line(ctx);
		out_append(ctx, ANSI_DIM);
		break;
	case MD_BLOCK_P:
		if (ctx->list_depth > 0 && ctx->li_first_para[ctx->list_depth - 1]) {
			ctx->li_first_para[ctx->list_depth - 1] = 0;
		} else {
			ensure_blank_line(ctx);
		}
		if (ctx->quote_depth > 0)
			start_content_line(ctx);
		if (!ctx->table || !ctx->table->cell_active) {
			morph_buf_cleanup(&ctx->inline_raw);
			if (morph_buf_init(&ctx->inline_raw, 128) == 0)
				ctx->inline_active = 1;
		}
		break;
	case MD_BLOCK_QUOTE:
		ensure_blank_line(ctx);
		ctx->quote_depth++;
		ctx->last_block_was_visible = 0;
		break;
	case MD_BLOCK_UL: {
		struct MD_BLOCK_UL_DETAIL *u = detail;
		(void)u;
		if (ctx->list_depth == 0)
			ensure_blank_line(ctx);
		else
			newline_with_prefix(ctx);
		if (ctx->list_depth < (int)(sizeof(ctx->list_is_ol) / sizeof(ctx->list_is_ol[0]))) {
			ctx->list_is_ol[ctx->list_depth] = 0;
			ctx->olist_index[ctx->list_depth] = 0;
			ctx->li_first_para[ctx->list_depth] = 0;
		}
		ctx->list_depth++;
		ctx->last_block_was_visible = 0;
		break;
	}
	case MD_BLOCK_OL: {
		struct MD_BLOCK_OL_DETAIL *o = detail;
		if (ctx->list_depth == 0)
			ensure_blank_line(ctx);
		else
			newline_with_prefix(ctx);
		if (ctx->list_depth < (int)(sizeof(ctx->list_is_ol) / sizeof(ctx->list_is_ol[0]))) {
			ctx->list_is_ol[ctx->list_depth] = 1;
			ctx->olist_index[ctx->list_depth] = (int)o->start - 1;
			ctx->li_first_para[ctx->list_depth] = 0;
		}
		ctx->list_depth++;
		ctx->last_block_was_visible = 0;
		break;
	}
	case MD_BLOCK_LI: {
		struct MD_BLOCK_LI_DETAIL *li = detail;
		int level = ctx->list_depth - 1;
		if (level >= 0) {
			start_content_line(ctx);
			for (int i = 0; i < level; i++)
				out_append(ctx, "  ");
			if (li && li->is_task) {
				out_append(ctx, "[");
				if (li->task_mark == 'x' || li->task_mark == 'X') {
					out_append(ctx, ANSI_GREEN);
					out_append(ctx, "x");
				} else {
					out_append(ctx, ANSI_GRAY);
					out_append(ctx, " ");
				}
				out_append(ctx, ANSI_RESET);
				reapply_inline(ctx);
				out_append(ctx, "] ");
			} else if (level >= 0 && level < (int)(sizeof(ctx->list_is_ol) / sizeof(ctx->list_is_ol[0])) &&
			    ctx->list_is_ol[level]) {
				char marker[16];
				ctx->olist_index[level]++;
				snprintf(marker, sizeof(marker), "%d. ", ctx->olist_index[level]);
				out_append(ctx, ANSI_BOLD);
				out_append(ctx, marker);
				out_append(ctx, ANSI_RESET);
				reapply_inline(ctx);
			} else {
				out_append(ctx, ANSI_BOLD);
				out_append(ctx, "• ");
				out_append(ctx, ANSI_RESET);
				reapply_inline(ctx);
			}
			if (level < (int)(sizeof(ctx->li_first_para) / sizeof(ctx->li_first_para[0])))
				ctx->li_first_para[level] = 1;
			ctx->last_block_was_visible = 0;
		}
		break;
	}
	case MD_BLOCK_HR:
		ensure_blank_line(ctx);
		out_append(ctx, ANSI_DIM);
		out_append(ctx, "────────────────────────────────────────");
		out_append(ctx, ANSI_RESET);
		ctx->last_block_was_visible = 1;
		break;

	case MD_BLOCK_TABLE: {
		struct MD_BLOCK_TABLE_DETAIL *td = detail;
		ensure_blank_line(ctx);
		struct table_state *t = calloc(1, sizeof(*t));
		if (!t)
			return 0;
		t->col_count = td->col_count;
		t->head_row_count = td->head_row_count;
		t->align = calloc(td->col_count, sizeof(MD_ALIGN));
		if (!t->align ||
		    morph_array_init(&t->rows, 4, sizeof(struct table_row)) != 0) {
			free(t->align);
			free(t);
			return 0;
		}
		ctx->table = t;
		break;
	}
	case MD_BLOCK_THEAD:
		if (ctx->table)
			ctx->table->in_head = 1;
		break;
	case MD_BLOCK_TBODY:
		if (ctx->table)
			ctx->table->in_head = 0;
		break;
	case MD_BLOCK_TR: {
		struct table_state *t = ctx->table;
		if (!t)
			break;
		struct table_row *row = morph_array_push(&t->rows);
		if (!row)
			break;
		memset(row, 0, sizeof(*row));
		row->cells = calloc(t->col_count, sizeof(struct table_cell));
		if (!row->cells) {
			morph_array_pop(&t->rows);
			break;
		}
		break;
	}
	case MD_BLOCK_TH:
	case MD_BLOCK_TD: {
		struct table_state *t = ctx->table;
		if (!t)
			break;
		struct MD_BLOCK_TD_DETAIL *cd = detail;
		if (t->rows.nelts == 0)
			break;
		struct table_row *row = &table_rows(t)[t->rows.nelts - 1];
		if (row->cell_count < t->col_count && t->in_head)
			t->align[row->cell_count] = cd->align;
		morph_buf_cleanup(&t->cell_raw);
		if (morph_buf_init(&t->cell_raw, 64) == 0)
			t->cell_active = 1;
		break;
	}
	default:
		break;
	}
	return 0;
}

/* Strip ANSI escape sequences, producing a plain-text copy.
 * Returns the byte length of the plain text. */
static size_t strip_ansi(const char *src, size_t len, char **out_plain)
{
	size_t out_len = 0;
	char *plain = utf8_strip_ansi_dup(src, len, &out_len);
	if (!plain) {
		if (out_plain)
			*out_plain = NULL;
		return 0;
	}
	*out_plain = plain;
	return out_len;
}

static size_t terminal_escape_end(const char *src, size_t len, size_t i)
{
	if (!src || i >= len || (unsigned char)src[i] != 0x1b ||
	    i + 1 >= len)
		return i;

	if (src[i + 1] == '[') {
		i += 2;
		while (i < len && !((src[i] >= 'A' && src[i] <= 'Z') ||
		       (src[i] >= 'a' && src[i] <= 'z')))
			i++;
		if (i < len)
			i++;
		return i;
	}

	if (src[i + 1] == ']' || src[i + 1] == '_') {
		char terminator = src[i + 1] == ']' ? '\a' : '\0';
		i += 2;
		while (i < len) {
			if (terminator && src[i] == terminator) {
				i++;
				break;
			}
			if (src[i] == '\033' && i + 1 < len &&
			    src[i + 1] == '\\') {
				i += 2;
				break;
			}
			i++;
		}
		return i;
	}

	return i + 2;
}

static size_t plain_max_line_width(const char *plain)
{
	size_t max_w = 0;
	size_t cur_w = 0;
	size_t len;
	size_t i = 0;

	if (!plain)
		return 0;

	len = strlen(plain);
	while (i < len) {
		unsigned cp;
		size_t cp_len;

		if (plain[i] == '\n') {
			if (cur_w > max_w)
				max_w = cur_w;
			cur_w = 0;
			i++;
			continue;
		}

		if (!utf8_decode_codepoint(plain + i, len - i, &cp, &cp_len))
			break;
		cur_w += (size_t)utf8_codepoint_width(cp);
		i += cp_len;
	}
	if (cur_w > max_w)
		max_w = cur_w;
	return max_w;
}

/* Wrap a single cell's raw (ANSI-decorated) content to fit within max_cols
 * visible columns. Produces an array of wrapped_line structs. Sets
 * *out_count to the number of lines. Caller must free each line->raw and
 * the returned array itself. */
static struct wrapped_line *wrap_cell_content(const char *raw, size_t raw_len,
					     const char *plain, size_t plain_len,
					     size_t max_cols,
					     unsigned *out_count)
{
	morph_array_t lines;

	*out_count = 0;
	if (max_cols == 0)
		max_cols = 1;

	size_t cap = plain_len + 1;
	if (cap < 4)
		cap = 4;
	if (morph_array_init(&lines, cap, sizeof(struct wrapped_line)) != 0)
		return NULL;

	size_t ri = 0;
	size_t pi = 0;
	size_t line_start = 0;
	size_t line_vis = 0;

	/* Track last break opportunity (after a space or CJK character) */
	size_t break_ri = 0;
	size_t break_pi = 0;
	size_t break_vis = 0;

	while (ri < raw_len) {
		if ((unsigned char)raw[ri] == 0x1b) {
			size_t end = terminal_escape_end(raw, raw_len, ri);
			if (end > ri) {
				ri = end;
				continue;
			}
		}

		if (pi < plain_len && plain[pi] == '\n') {
			struct wrapped_line *line = morph_array_push(&lines);
			size_t seg_len;

			if (!line)
				goto fail;
			memset(line, 0, sizeof(*line));
			seg_len = ri > line_start ? ri - line_start : 0;
			line->raw = malloc(seg_len + 1);
			if (!line->raw)
				goto fail;
			if (seg_len > 0)
				memcpy(line->raw, raw + line_start, seg_len);
			line->raw[seg_len] = '\0';
			line->raw_len = seg_len;
			line->vis_width = line_vis;

			if (ri < raw_len && raw[ri] == '\n')
				ri++;
			pi++;
			line_start = ri;
			line_vis = 0;
			break_ri = line_start;
			break_pi = pi;
			break_vis = 0;
			continue;
		}

		unsigned cp;
		size_t char_bytes;
		if (!utf8_decode_codepoint(plain + pi, plain_len - pi, &cp,
				 &char_bytes))
			break;
		size_t char_cols = (size_t)utf8_codepoint_width(cp);
		int is_cjk = utf8_is_cjk_cp(cp);
		int is_space = ((unsigned char)plain[pi] == ' ');

		if (line_vis + char_cols > max_cols && line_vis > 0) {
			/* Overflow: break at last break opportunity if available.
			 * If we break at an earlier point, rewind ri/pi back to
			 * that point so any already-consumed chars after the
			 * break get re-processed on the next line — otherwise
			 * their visible width would be lost. */
			size_t seg_ri, seg_vis;
			if (break_ri > line_start && break_vis > 0) {
				seg_ri = break_ri;
				seg_vis = break_vis;
				ri = break_ri;
				pi = break_pi;
			} else {
				seg_ri = ri;
				seg_vis = line_vis;
			}

			struct wrapped_line *line = morph_array_push(&lines);
			if (!line)
				goto fail;
			memset(line, 0, sizeof(*line));
			size_t seg_len = seg_ri - line_start;
			line->raw = malloc(seg_len + 1);
			if (!line->raw)
				goto fail;
			memcpy(line->raw, raw + line_start, seg_len);
			line->raw[seg_len] = '\0';
			line->raw_len = seg_len;
			line->vis_width = seg_vis;

			line_start = seg_ri;
			line_vis = 0;
			break_ri = line_start;
			break_pi = pi;
			break_vis = 0;

			/* Skip leading spaces on new line */
			while (ri < raw_len && pi < plain_len &&
			       (unsigned char)plain[pi] == ' ') {
				unsigned skip_cp;
				size_t skip_bytes;

				if (!utf8_decode_codepoint(plain + pi, plain_len - pi,
						 &skip_cp, &skip_bytes))
					break;
				char_cols = (size_t)utf8_codepoint_width(skip_cp);
				ri += skip_bytes;
				pi += skip_bytes;
				line_start = ri;
			}
			/* Recalculate current character after skip */
			if (ri >= raw_len)
				break;
			unsigned cp2;
			if (!utf8_decode_codepoint(plain + pi, plain_len - pi, &cp2,
					 &char_bytes))
				break;
			char_cols = (size_t)utf8_codepoint_width(cp2);
			is_cjk = utf8_is_cjk_cp(cp2);
			is_space = 0;
			line_vis = 0;
			break_ri = line_start;
			break_pi = pi;
			break_vis = 0;
		}

		/* Record break opportunity after spaces and CJK characters */
		if (is_space || is_cjk) {
			break_ri = ri + char_bytes;
			break_pi = pi + char_bytes;
			break_vis = 0;
		}

		ri += char_bytes;
		pi += char_bytes;
		line_vis += char_cols;

		/* Update break_vis to reflect accumulated width at the break point */
		if (break_vis == 0 && (is_space || is_cjk)) {
			break_vis = line_vis;
		}
	}

	/* Flush remaining content as the last sub-line */
	if (ri > line_start || lines.nelts == 0) {
		struct wrapped_line *line = morph_array_push(&lines);
		if (!line)
			goto fail;
		memset(line, 0, sizeof(*line));
		size_t seg_len = ri - line_start;
		line->raw = malloc(seg_len + 1);
		if (!line->raw)
			goto fail;
		memcpy(line->raw, raw + line_start, seg_len);
		line->raw[seg_len] = '\0';
		line->raw_len = seg_len;
		line->vis_width = line_vis;
	}

	if (lines.nelts > UINT_MAX)
		goto fail;
	*out_count = (unsigned)lines.nelts;
	{
		struct wrapped_line *ret = lines.elts;
		lines.elts = NULL;
		lines.nelts = 0;
		lines.cap = 0;
		lines.size = 0;
		lines.heap_alloc = 0;
		return ret;
	}

fail:
	{
		struct wrapped_line *items = lines.elts;
		for (size_t k = 0; k < lines.nelts; k++)
			free(items[k].raw);
		morph_array_cleanup(&lines);
		return NULL;
	}
}

static void free_wrapped_lines(struct wrapped_line *lines, unsigned count)
{
	if (!lines)
		return;
	for (unsigned i = 0; i < count; i++)
		free(lines[i].raw);
	free(lines);
}

static struct wrapped_line *split_buf_lines(char *data, size_t len,
					    unsigned *out_count)
{
	morph_array_t lines;
	size_t start = 0;

	*out_count = 0;
	if (morph_array_init(&lines, 4, sizeof(struct wrapped_line)) != 0) {
		free(data);
		return NULL;
	}

	for (size_t i = 0; i <= len; i++) {
		if (i == len || data[i] == '\n') {
			struct wrapped_line *line;
			size_t seg_len = i - start;
			char *plain = NULL;
			size_t plain_len;

			if (i == len && seg_len == 0 && lines.nelts > 0)
				break;
			line = morph_array_push(&lines);
			if (!line)
				goto fail;
			memset(line, 0, sizeof(*line));
			line->raw = malloc(seg_len + 1);
			if (!line->raw)
				goto fail;
			if (seg_len > 0)
				memcpy(line->raw, data + start, seg_len);
			line->raw[seg_len] = '\0';
			line->raw_len = seg_len;
			plain_len = strip_ansi(line->raw, line->raw_len, &plain);
			(void)plain_len;
			line->vis_width = plain ? utf8_display_width(plain) : 0;
			free(plain);
			start = i + 1;
		}
	}

	if (lines.nelts > UINT_MAX)
		goto fail;
	free(data);
	*out_count = (unsigned)lines.nelts;
	{
		struct wrapped_line *ret = lines.elts;
		lines.elts = NULL;
		lines.nelts = 0;
		lines.cap = 0;
		lines.size = 0;
		lines.heap_alloc = 0;
		return ret;
	}

fail:
	{
		struct wrapped_line *items = lines.elts;
		for (size_t k = 0; k < lines.nelts; k++)
			free(items[k].raw);
		morph_array_cleanup(&lines);
		free(data);
		return NULL;
	}
}

static struct wrapped_line *layout_cell_content(struct ansi_ctx *ctx,
						const char *raw,
						size_t raw_len,
						size_t max_cols,
						unsigned *out_count)
{
	morph_buf_t out;
	char *data;
	size_t len;

	*out_count = 0;
	if (morph_buf_init(&out, raw_len + 64) != 0)
		return NULL;
	render_inline_layout(ctx, raw, raw_len, max_cols, 1, &out);
	len = out.len;
	data = morph_buf_detach(&out);
	return split_buf_lines(data, len, out_count);
}

static int table_math_delim_at(const char *raw, size_t raw_len, size_t pos,
			       int *display, size_t *delim_len)
{
	if (!raw || pos >= raw_len || raw[pos] != '$')
		return 0;
	if (pos + 1 < raw_len && raw[pos + 1] == '$') {
		if (display)
			*display = 1;
		if (delim_len)
			*delim_len = 2;
		return 1;
	}
	if (display)
		*display = 0;
	if (delim_len)
		*delim_len = 1;
	return 1;
}

static int table_find_math_end(const char *raw, size_t raw_len,
			       size_t content_start, size_t delim_len,
			       size_t *content_end, size_t *after_end)
{
	size_t i = content_start;

	while (i < raw_len) {
		if (raw[i] == '\\' && i + 1 < raw_len) {
			i += 2;
			continue;
		}
		if ((unsigned char)raw[i] == 0x1b) {
			size_t end = terminal_escape_end(raw, raw_len, i);
			i = end > i ? end : i + 1;
			continue;
		}
		if (raw[i] == '$') {
			if (delim_len == 2) {
				if (i + 1 < raw_len && raw[i + 1] == '$') {
					if (content_end)
						*content_end = i;
					if (after_end)
						*after_end = i + 2;
					return 1;
				}
			} else {
				if (content_end)
					*content_end = i;
				if (after_end)
					*after_end = i + 1;
				return 1;
			}
		}
		i++;
	}
	return 0;
}

static void process_table_cell_math(struct ansi_ctx *ctx, morph_buf_t *cell)
{
	char *raw;
	size_t raw_len;
	size_t seg_start = 0;
	size_t i = 0;
	int in_code = 0;

	if (!ctx || !cell || !cell->data || !memchr(cell->data, '$', cell->len))
		return;

	raw_len = cell->len;
	raw = morph_buf_detach(cell);
	if (!raw)
		return;
	if (morph_buf_init(cell, raw_len + 64) != 0) {
		free(raw);
		return;
	}

	while (i < raw_len) {
		if ((unsigned char)raw[i] == 0x1b) {
			size_t end = terminal_escape_end(raw, raw_len, i);

			if (i + 5 <= raw_len &&
			    memcmp(raw + i, ANSI_CYAN, 5) == 0)
				in_code = 1;
			else if (i + 4 <= raw_len &&
				 memcmp(raw + i, ANSI_RESET, 4) == 0)
				in_code = 0;
			i = end > i ? end : i + 1;
			continue;
		}

		if (!in_code && raw[i] == '$') {
			int display;
			size_t delim_len;
			size_t content_start;
			size_t content_end;
			size_t after_end;

			if (table_math_delim_at(raw, raw_len, i, &display,
						&delim_len)) {
				content_start = i + delim_len;
				if (table_find_math_end(raw, raw_len,
							content_start,
							delim_len,
							&content_end,
							&after_end) &&
				    content_end > content_start) {
					out_append_n(ctx, raw + seg_start,
						     i - seg_start);
					if (render_mathjax_kitty(ctx,
							raw + content_start,
							content_end -
							content_start,
							display) != 0) {
						out_append_n(ctx, raw + i,
							     after_end - i);
					}
					i = after_end;
					seg_start = i;
					continue;
				}
			}
		}
		i++;
	}

	out_append_n(ctx, raw + seg_start, raw_len - seg_start);
	free(raw);
}

/* Calculate column widths constrained to terminal width using water-filling.
 *
 * Water-filling: iteratively raise a "water level" = remaining_space /
 * unfixed_count.  Columns whose natural width ≤ level are fixed at their
 * natural width (narrow columns stay narrow — no wasted space).  The
 * remaining space is shared equally among the still-unfixed (wider)
 * columns.  This produces balanced output where wide columns shrink
 * proportionally and narrow columns are never inflated or crushed.
 *
 * Example: natural [50, 12, 12, 8], avail 60
 *   Round 1: target = 60/4 = 15 → cols 1,2,3 ≤ 15 → fixed at 12,12,8
 *   Round 2: remaining = 60-32 = 28, unfixed = 1 → col 0 = 28
 *   Result:  [28, 12, 12, 8]
 */
static void distribute_col_widths(size_t *col_w, unsigned col_count,
				  size_t prefix_width, int term_width)
{
	if (!col_w || col_count == 0)
		return;

	size_t border_overhead = (size_t)col_count * 2 + 1 + col_count;
	size_t total_natural = 0;
	for (unsigned c = 0; c < col_count; c++)
		total_natural += col_w[c];

	size_t total_width = prefix_width + border_overhead + total_natural;

	if (term_width <= 0 || total_width <= (size_t)term_width)
		return;

	size_t avail = (size_t)term_width;
	if (avail < prefix_width + border_overhead)
		return;
	size_t avail_content = avail - prefix_width - border_overhead;

	size_t min_w = 4;
	size_t total_min = (size_t)col_count * min_w;
	if (avail_content < total_min) {
		size_t each = avail_content / col_count;
		for (unsigned c = 0; c < col_count; c++)
			col_w[c] = each;
		return;
	}

	/* Save natural widths; we'll write results back into col_w */
	size_t *natural = malloc(col_count * sizeof(size_t));
	if (!natural)
		return;
	for (unsigned c = 0; c < col_count; c++)
		natural[c] = col_w[c];

	int *fixed = calloc(col_count, sizeof(int));
	if (!fixed) {
		free(natural);
		return;
	}

	size_t remaining = avail_content;
	unsigned unfixed = col_count;

	/* Iteratively find the water level */
	for (unsigned iter = 0; iter < col_count; iter++) {
		if (unfixed == 0)
			break;
		size_t target = remaining / unfixed;

		int progress = 0;
		for (unsigned c = 0; c < col_count; c++) {
			if (fixed[c])
				continue;
			if (natural[c] <= target) {
				col_w[c] = natural[c];
				remaining -= natural[c];
				fixed[c] = 1;
				unfixed--;
				progress = 1;
			}
		}
		if (!progress)
			break;
	}

	/* Distribute remaining space among unfixed columns */
	if (unfixed > 0) {
		size_t each = remaining / unfixed;
		size_t extra = remaining - each * unfixed;
		for (unsigned c = 0; c < col_count; c++) {
			if (fixed[c])
				continue;
			col_w[c] = each;
			if (extra > 0) {
				col_w[c]++;
				extra--;
			}
		}
	}

	/* Enforce minimum width */
	for (unsigned c = 0; c < col_count; c++) {
		if (col_w[c] < min_w)
			col_w[c] = min_w;
	}

	free(fixed);
	free(natural);
}

static void render_table(struct ansi_ctx *ctx, struct table_state *t)
{
	if (!t || t->col_count == 0 || t->rows.nelts == 0)
		return;

	size_t *col_w = calloc(t->col_count, sizeof(size_t));
	if (!col_w)
		return;
	struct table_row *rows = table_rows(t);

	for (size_t r = 0; r < t->rows.nelts; r++) {
		struct table_row *row = &rows[r];
		for (unsigned c = 0; c < row->cell_count && c < t->col_count; c++) {
			size_t w;

			if (inline_raw_has_formula(row->cells[c].raw,
						   row->cells[c].raw_len)) {
				w = inline_raw_natural_width(ctx,
					row->cells[c].raw,
					row->cells[c].raw_len);
			} else {
				w = plain_max_line_width(row->cells[c].plain);
			}
			if (w > col_w[c])
				col_w[c] = w;
		}
	}

	/* Constrain column widths to terminal width */
	size_t prefix_width = (size_t)(ctx->quote_depth * 2);
	distribute_col_widths(col_w, t->col_count, prefix_width,
			      ctx->term_width);

	/* Wrap each cell content to its constrained column width */
	unsigned **wrap_counts = calloc(t->rows.nelts, sizeof(unsigned *));
	struct wrapped_line ***wrap_lines =
		calloc(t->rows.nelts, sizeof(struct wrapped_line **));
	if (!wrap_counts || !wrap_lines) {
		free(col_w);
		free(wrap_counts);
		free(wrap_lines);
		return;
	}

	for (size_t r = 0; r < t->rows.nelts; r++) {
		struct table_row *row = &rows[r];
		wrap_counts[r] = calloc(t->col_count, sizeof(unsigned));
		wrap_lines[r] = calloc(t->col_count, sizeof(struct wrapped_line *));
		for (unsigned c = 0; c < row->cell_count && c < t->col_count; c++) {
			if (inline_raw_has_formula(row->cells[c].raw,
						   row->cells[c].raw_len)) {
				wrap_lines[r][c] = layout_cell_content(
					ctx, row->cells[c].raw,
					row->cells[c].raw_len, col_w[c],
					&wrap_counts[r][c]);
			} else {
				wrap_lines[r][c] = wrap_cell_content(
					row->cells[c].raw,
					row->cells[c].raw_len,
					row->cells[c].plain,
					row->cells[c].plain_len,
					col_w[c], &wrap_counts[r][c]);
			}
		}
	}

	for (size_t r = 0; r < t->rows.nelts; r++) {
		struct table_row *row = &rows[r];
		/* Find max number of wrapped lines for this data row */
		unsigned max_lines = 1;
		for (unsigned c = 0; c < t->col_count; c++) {
			if (c < row->cell_count && wrap_counts[r] &&
			    wrap_counts[r][c] > max_lines)
				max_lines = wrap_counts[r][c];
		}

		for (unsigned ln = 0; ln < max_lines; ln++) {
			if (r > 0 || ln > 0)
				newline_with_prefix(ctx);
			else
				start_content_line(ctx);

			for (unsigned c = 0; c < t->col_count; c++) {
				out_append(ctx, ANSI_DIM);
				out_append(ctx, "│ ");
				out_append(ctx, ANSI_RESET);
				reapply_inline(ctx);

				struct wrapped_line *cell_lines =
					(c < row->cell_count && wrap_lines[r])
						? wrap_lines[r][c] : NULL;
				unsigned cell_nlines =
					(c < row->cell_count && wrap_counts[r])
						? wrap_counts[r][c] : 0;

				const char *seg_raw = "";
				size_t seg_raw_len = 0;
				size_t seg_vis = 0;

				if (ln < cell_nlines && cell_lines) {
					seg_raw = cell_lines[ln].raw;
					seg_raw_len = cell_lines[ln].raw_len;
					seg_vis = cell_lines[ln].vis_width;
				}

				size_t target_w = col_w[c];
				size_t pad = target_w > seg_vis ? target_w - seg_vis : 0;

				MD_ALIGN al = t->align[c];
				size_t left = 0, right = 0;
				if (al == MD_ALIGN_RIGHT) left = pad;
				else if (al == MD_ALIGN_CENTER) {
					left = pad / 2;
					right = pad - left;
				} else {
					right = pad;
				}

				for (size_t i = 0; i < left; i++)
					out_append(ctx, " ");
				if (r == 0)
					out_append(ctx, ANSI_BOLD);
				out_append_n(ctx, seg_raw, seg_raw_len);
				if (r == 0) {
					out_append(ctx, ANSI_RESET);
					reapply_inline(ctx);
				}
				for (size_t i = 0; i < right; i++)
					out_append(ctx, " ");
				out_append(ctx, " ");
			}
			out_append(ctx, ANSI_DIM);
			out_append(ctx, "│");
			out_append(ctx, ANSI_RESET);
			reapply_inline(ctx);
		}

		if (r == 0 && t->head_row_count > 0) {
			newline_with_prefix(ctx);
			out_append(ctx, ANSI_DIM);
			for (unsigned c = 0; c < t->col_count; c++) {
				out_append(ctx, "├─");
				for (size_t i = 0; i < col_w[c] + 1; i++)
					out_append(ctx, "─");
			}
			out_append(ctx, "┤");
			out_append(ctx, ANSI_RESET);
			reapply_inline(ctx);
		}
	}

	/* Free wrapped lines */
	for (size_t r = 0; r < t->rows.nelts; r++) {
		if (wrap_lines[r]) {
			for (unsigned c = 0; c < t->col_count &&
			     c < rows[r].cell_count; c++) {
				if (wrap_lines[r][c])
					free_wrapped_lines(wrap_lines[r][c],
							   wrap_counts ? wrap_counts[r][c] : 0);
			}
			free(wrap_lines[r]);
		}
		if (wrap_counts && wrap_counts[r])
			free(wrap_counts[r]);
	}
	free(wrap_lines);
	free(wrap_counts);
	free(col_w);
	ctx->last_block_was_visible = 1;
}

static void free_table(struct table_state *t)
{
	if (!t)
		return;
	struct table_row *rows = table_rows(t);
	for (size_t r = 0; r < t->rows.nelts; r++) {
		for (unsigned c = 0; c < rows[r].cell_count; c++) {
			free(rows[r].cells[c].raw);
			free(rows[r].cells[c].plain);
		}
		free(rows[r].cells);
	}
	morph_array_cleanup(&t->rows);
	free(t->align);
	morph_buf_cleanup(&t->cell_raw);
	free(t);
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
	(void)detail;
	struct ansi_ctx *ctx = userdata;
	switch (type) {
	case MD_BLOCK_DOC:
		break;
	case MD_BLOCK_H:
		ctx->in_heading = 0;
		out_append(ctx, ANSI_RESET);
		ctx->last_block_was_visible = 1;
		break;
	case MD_BLOCK_CODE: {
		ctx->in_code_block = 0;
		/* highlight buffered code */
		struct sbuf hl;
		char hlbuf[16384];
		sbuf_init(&hl, hlbuf, sizeof(hlbuf));
		highlight_code(ctx->code_lang, ctx->code_lang_len,
			       ctx->code_raw.data ? ctx->code_raw.data : "",
			       ctx->code_raw.len,
			       &hl, NULL, NULL);
		/* emit highlighted code with line prefix handling */
		const char *htext = hl.buf ? hl.buf :
				    (ctx->code_raw.data ? ctx->code_raw.data : "");
		size_t hlen = hl.buf ? hl.len : ctx->code_raw.len;
		size_t start = 0;
		for (size_t j = 0; j < hlen; j++) {
			if (htext[j] == '\n') {
				out_append_n(ctx, htext + start, j - start);
				out_append(ctx, ANSI_RESET);
				newline_with_prefix(ctx);
				start = j + 1;
			}
		}
		if (start < hlen)
			out_append_n(ctx, htext + start, hlen - start);
		morph_buf_cleanup(&ctx->code_raw);
		out_append(ctx, ANSI_RESET);
		newline_with_prefix(ctx);
		out_append(ctx, ANSI_DIM);
		out_append(ctx, "```");
		out_append(ctx, ANSI_RESET);
		ctx->code_depth--;
		ctx->last_block_was_visible = 1;
		break;
	}
	case MD_BLOCK_HTML:
		out_append(ctx, ANSI_RESET);
		ctx->last_block_was_visible = 1;
		break;
	case MD_BLOCK_P:
		if (ctx->inline_active) {
			ctx->inline_active = 0;
			render_inline_layout(ctx,
					     ctx->inline_raw.data ?
					     ctx->inline_raw.data : "",
					     ctx->inline_raw.len,
					     ctx->term_width > 0 ?
					     (size_t)ctx->term_width : 80,
					     0, NULL);
			morph_buf_cleanup(&ctx->inline_raw);
		}
		ctx->last_block_was_visible = 1;
		break;
	case MD_BLOCK_QUOTE:
		ctx->quote_depth--;
		ctx->last_block_was_visible = 1;
		break;
	case MD_BLOCK_UL:
	case MD_BLOCK_OL:
		ctx->list_depth--;
		ctx->last_block_was_visible = 1;
		break;
	case MD_BLOCK_LI:
		ctx->last_block_was_visible = 1;
		break;
	case MD_BLOCK_HR:
		break;
	case MD_BLOCK_TH:
	case MD_BLOCK_TD: {
		struct table_state *t = ctx->table;
		if (!t || !t->cell_active)
			break;
		if (t->rows.nelts == 0)
			break;
		struct table_row *row = &table_rows(t)[t->rows.nelts - 1];
		if (row->cell_count < t->col_count) {
			struct table_cell *cell = &row->cells[row->cell_count];
			process_table_cell_math(ctx, &t->cell_raw);
			cell->raw_len = t->cell_raw.len;
			cell->raw = morph_buf_detach(&t->cell_raw);
			if (!cell->raw) {
				cell->raw = strdup("");
				cell->raw_len = 0;
			}
			if (!cell->raw) {
				t->cell_active = 0;
				break;
			}
			cell->plain_len = strip_ansi(cell->raw, cell->raw_len,
						     &cell->plain);
			row->cell_count++;
		} else {
			morph_buf_cleanup(&t->cell_raw);
		}
		t->cell_active = 0;
		break;
	}
	case MD_BLOCK_TR:
		break;
	case MD_BLOCK_THEAD:
	case MD_BLOCK_TBODY:
		break;
	case MD_BLOCK_TABLE: {
		struct table_state *t = ctx->table;
		ctx->table = NULL;
		render_table(ctx, t);
		free_table(t);
		break;
	}
	default:
		break;
	}
	return 0;
}

static int is_video_ext(const char *path, size_t len)
{
	if (len > 4) {
		const char *ext = path + len - 4;
		if (strcmp(ext, ".mp4") == 0 ||
		    strcmp(ext, ".mov") == 0 ||
		    strcmp(ext, ".avi") == 0 ||
		    strcmp(ext, ".mkv") == 0 ||
		    strcmp(ext, ".m4v") == 0)
			return 1;
	}
	if (len > 5) {
		const char *ext5 = path + len - 5;
		if (strcmp(ext5, ".webm") == 0 ||
		    strcmp(ext5, ".mpeg") == 0)
			return 1;
	}
	return 0;
}

static void collect_media(struct ansi_ctx *ctx, const char *src, size_t src_len, const char *default_type)
{
	if (ctx->media_count >= 64)
		return;
	const char *s = src;
	size_t slen = src_len;
	if (slen > 7 && strncmp(s, "file://", 7) == 0) {
		s += 7;
		slen -= 7;
	}
	char *path = malloc(slen + 1);
	if (!path)
		return;
	memcpy(path, s, slen);
	path[slen] = '\0';
	const char *type = is_video_ext(path, slen) ? "video" : default_type;
	ctx->media[ctx->media_count].type = strdup(type);
	ctx->media[ctx->media_count].path = path;
	ctx->media_count++;
}

/* ---------------- span handlers ---------------- */
static void append_attr(struct ansi_ctx *ctx, const MD_ATTRIBUTE *attr)
{
	if (!attr || !attr->text)
		return;
	out_append_n(ctx, attr->text, attr->size);
}

static const char mathjax_b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static unsigned char mathjax_pixel_byte(const uint32_t *pixels, size_t offset)
{
        uint32_t p = pixels[offset / 4];

        switch (offset % 4) {
        case 0:
                return (unsigned char)((p >> 24) & 0xffu);
        case 1:
                return (unsigned char)((p >> 16) & 0xffu);
        case 2:
                return (unsigned char)((p >> 8) & 0xffu);
        default:
                return (unsigned char)(p & 0xffu);
        }
}

static void append_mathjax_b64(struct ansi_ctx *ctx, const uint32_t *pixels,
                               size_t offset, size_t len)
{
        size_t i = 0;
        char out[4];

        while (i < len) {
                unsigned int a = mathjax_pixel_byte(pixels, offset + i);
                unsigned int b = 0;
                unsigned int c = 0;
                size_t rem = len - i;

                i++;
                if (rem > 1) {
                        b = mathjax_pixel_byte(pixels, offset + i);
                        i++;
                }
                if (rem > 2) {
                        c = mathjax_pixel_byte(pixels, offset + i);
                        i++;
                }

                out[0] = mathjax_b64[(a >> 2) & 0x3fu];
                out[1] = mathjax_b64[((a & 0x03u) << 4) |
                                      ((b >> 4) & 0x0fu)];
                out[2] = rem > 1 ? mathjax_b64[((b & 0x0fu) << 2) |
                                                ((c >> 6) & 0x03u)] : '=';
                out[3] = rem > 2 ? mathjax_b64[c & 0x3fu] : '=';
                out_append_n(ctx, out, sizeof(out));
        }
}

static unsigned char mathjax_pixel_byte_raw(const uint32_t *pixels,
					    size_t offset)
{
	return mathjax_pixel_byte(pixels, offset);
}

static int append_mathjax_b64_buf(morph_buf_t *out, const uint32_t *pixels,
				  size_t offset, size_t len)
{
	size_t i = 0;
	char enc[4];

	while (i < len) {
		unsigned int a = mathjax_pixel_byte_raw(pixels, offset + i);
		unsigned int b = 0;
		unsigned int c = 0;
		size_t rem = len - i;

		i++;
		if (rem > 1) {
			b = mathjax_pixel_byte_raw(pixels, offset + i);
			i++;
		}
		if (rem > 2) {
			c = mathjax_pixel_byte_raw(pixels, offset + i);
			i++;
		}

		enc[0] = mathjax_b64[(a >> 2) & 0x3fu];
		enc[1] = mathjax_b64[((a & 0x03u) << 4) |
				      ((b >> 4) & 0x0fu)];
		enc[2] = rem > 1 ? mathjax_b64[((b & 0x0fu) << 2) |
						((c >> 6) & 0x03u)] : '=';
		enc[3] = rem > 2 ? mathjax_b64[c & 0x3fu] : '=';
		if (morph_buf_append(out, enc, sizeof(enc)) != 0)
			return -ENOMEM;
	}
	return 0;
}

static struct formula_atom *formula_lookup(struct ansi_ctx *ctx,
					   unsigned int id)
{
	if (!ctx || !ctx->formulas_init)
		return NULL;
	struct formula_atom *items = ctx->formulas.elts;
	for (size_t i = 0; i < ctx->formulas.nelts; i++) {
		if (items[i].id == id)
			return &items[i];
	}
	return NULL;
}

static void free_formulas(struct ansi_ctx *ctx)
{
	if (!ctx || !ctx->formulas_init)
		return;
	struct formula_atom *items = ctx->formulas.elts;
	for (size_t i = 0; i < ctx->formulas.nelts; i++)
		free(items[i].transfer);
	morph_array_cleanup(&ctx->formulas);
	ctx->formulas_init = 0;
}

static void append_formula_marker(struct ansi_ctx *ctx, unsigned int id)
{
	char marker[32];
	int n = snprintf(marker, sizeof(marker), FORMULA_MARKER "%u\x1f", id);

	if (n > 0 && (size_t)n < sizeof(marker))
		out_append_n(ctx, marker, (size_t)n);
}

static int store_formula_atom(struct ansi_ctx *ctx, char *transfer,
			      size_t transfer_len, unsigned int cols,
			      unsigned int rows, unsigned int image_id,
			      int use_placeholder, unsigned int *out_id)
{
	struct formula_atom *atom;

	if (!ctx || !transfer || !out_id)
		return -EINVAL;
	if (!ctx->formulas_init) {
		if (morph_array_init(&ctx->formulas, 8,
				     sizeof(struct formula_atom)) != 0)
			return -ENOMEM;
		ctx->formulas_init = 1;
	}

	atom = morph_array_push(&ctx->formulas);
	if (!atom)
		return -ENOMEM;
	memset(atom, 0, sizeof(*atom));
	atom->id = ++ctx->next_formula_id;
	if (atom->id == 0)
		atom->id = ++ctx->next_formula_id;
	atom->transfer = transfer;
	atom->transfer_len = transfer_len;
	atom->cols = cols;
	atom->rows = rows;
	atom->image_id = image_id;
	atom->use_placeholder = use_placeholder;
	*out_id = atom->id;
	return 0;
}

static char *build_formula_transfer(const uint32_t *pixels, size_t bytes,
				    unsigned int width, unsigned int height,
				    unsigned int cols, unsigned int rows,
				    unsigned int image_id, int use_placeholder,
				    size_t *out_len)
{
	morph_buf_t out;
	size_t offset = 0;
	const size_t chunk_size = 3000;

	if (morph_buf_init(&out, 4096) != 0)
		return NULL;

	while (offset < bytes) {
		size_t remaining = bytes - offset;
		size_t n = remaining < chunk_size ? remaining : chunk_size;
		int more = offset + n < bytes ? 1 : 0;
		int rc;

		if (offset == 0) {
			if (use_placeholder) {
				rc = morph_buf_printf(&out,
					"\033_Ga=T,f=32,s=%u,v=%u,i=%u,U=1,c=%u,r=%u,q=2,m=%d;",
					width, height, image_id, cols, rows, more);
			} else {
				rc = morph_buf_printf(&out,
					"\033_Ga=T,f=32,s=%u,v=%u,c=%u,r=%u,C=1,z=0,m=%d;",
					width, height, cols, rows, more);
			}
		} else {
			rc = morph_buf_printf(&out, "\033_Gm=%d;", more);
		}
		if (rc != 0 ||
		    append_mathjax_b64_buf(&out, pixels, offset, n) != 0 ||
		    morph_buf_puts(&out, "\033\\") != 0) {
			morph_buf_cleanup(&out);
			return NULL;
		}
		offset += n;
	}

	if (out_len)
		*out_len = out.len;
	return morph_buf_detach(&out);
}

static unsigned int mathjax_cell_count(unsigned int pixels, unsigned int cell_px)
{
        unsigned int cells;

        if (cell_px == 0)
                return 1;
        cells = (pixels + cell_px - 1) / cell_px;
        return cells > 0 ? cells : 1;
}

static void append_spaces(struct ansi_ctx *ctx, unsigned int count)
{
        for (unsigned int i = 0; i < count; i++)
                out_append(ctx, " ");
}

static void append_mathjax_cursor_advance(struct ansi_ctx *ctx,
					  unsigned int cols,
					  unsigned int rows,
					  int display);

static void append_utf8_cp(struct ansi_ctx *ctx, unsigned int cp)
{
	char tmp[8];
	utf8_int8_t *end;
	size_t n;

	memset(tmp, 0, sizeof(tmp));
	end = utf8catcodepoint((utf8_int8_t *)tmp, (utf8_int32_t)cp,
			       sizeof(tmp));
	if (!end)
		return;
	n = (size_t)((char *)end - tmp);
	if (n > 0 && n < sizeof(tmp))
		out_append_n(ctx, tmp, n);
}

static int kitty_placeholder_supported(unsigned int cols, unsigned int rows,
				       unsigned int image_id)
{
	return cols > 0 && rows > 0 &&
	       cols <= KITTY_MAX_DIACRITIC_NUM + 1u &&
	       rows <= KITTY_MAX_DIACRITIC_NUM + 1u &&
	       image_id > 0 && image_id <= 0xffffffu;
}

static void append_kitty_placeholder_cell(struct ansi_ctx *ctx,
					  unsigned int row,
					  unsigned int col)
{
	append_utf8_cp(ctx, KITTY_PLACEHOLDER_CP);
	append_utf8_cp(ctx, kitty_num_to_diacritic[row]);
	append_utf8_cp(ctx, kitty_num_to_diacritic[col]);
}

static void append_kitty_placeholder_grid(struct ansi_ctx *ctx,
					  unsigned int image_id,
					  unsigned int cols,
					  unsigned int rows,
					  int display)
{
	char sgr[64];
	int n;
	unsigned int r = (image_id >> 16) & 0xffu;
	unsigned int g = (image_id >> 8) & 0xffu;
	unsigned int b = image_id & 0xffu;

	n = snprintf(sgr, sizeof(sgr), "\033[38;2;%u;%u;%um", r, g, b);
	if (n < 0 || (size_t)n >= sizeof(sgr)) {
		append_mathjax_cursor_advance(ctx, cols, rows, display);
		return;
	}
	out_append_n(ctx, sgr, (size_t)n);
	for (unsigned int y = 0; y < rows; y++) {
		if (y > 0)
			out_append_n(ctx, sgr, (size_t)n);
		for (unsigned int x = 0; x < cols; x++)
			append_kitty_placeholder_cell(ctx, y, x);
		if (display && y + 1 < rows) {
			if (ctx->table && ctx->table->cell_active)
				out_append(ctx, "\n");
			else {
				out_append(ctx, "\033[39m");
				newline_with_prefix(ctx);
			}
		}
	}
	out_append(ctx, "\033[39m");
	if (display) {
		if (ctx->table && ctx->table->cell_active)
			out_append(ctx, "\n");
		else
			newline_with_prefix(ctx);
	}
}

struct inline_run {
	int is_formula;
	const char *raw;
	size_t raw_len;
	struct formula_atom *atom;
	size_t cols;
	unsigned int rows;
	unsigned int baseline;
};

static void layout_append(struct ansi_ctx *ctx, int to_table_cell,
			  morph_buf_t *cell_out, const char *src, size_t len)
{
	if (to_table_cell && cell_out) {
		morph_buf_append(cell_out, src, len);
		return;
	}
	out_append_n(ctx, src, len);
}

static void layout_append_cstr(struct ansi_ctx *ctx, int to_table_cell,
			       morph_buf_t *cell_out, const char *src)
{
	layout_append(ctx, to_table_cell, cell_out, src, strlen(src));
}

static void layout_newline(struct ansi_ctx *ctx, int to_table_cell,
			   morph_buf_t *cell_out)
{
	if (to_table_cell) {
		layout_append_cstr(ctx, to_table_cell, cell_out, "\n");
		return;
	}
	newline_with_prefix(ctx);
}

static void layout_append_spaces(struct ansi_ctx *ctx, int to_table_cell,
				 morph_buf_t *cell_out, size_t count)
{
	for (size_t i = 0; i < count; i++)
		layout_append_cstr(ctx, to_table_cell, cell_out, " ");
}

static void layout_append_utf8_cp(struct ansi_ctx *ctx, int to_table_cell,
				  morph_buf_t *cell_out, unsigned int cp)
{
	char tmp[8];
	utf8_int8_t *end;
	size_t n;

	memset(tmp, 0, sizeof(tmp));
	end = utf8catcodepoint((utf8_int8_t *)tmp, (utf8_int32_t)cp,
			       sizeof(tmp));
	if (!end)
		return;
	n = (size_t)((char *)end - tmp);
	if (n > 0 && n < sizeof(tmp))
		layout_append(ctx, to_table_cell, cell_out, tmp, n);
}

static void layout_append_kitty_placeholder_cell(struct ansi_ctx *ctx,
						 int to_table_cell,
						 morph_buf_t *cell_out,
						 unsigned int row,
						 unsigned int col)
{
	layout_append_utf8_cp(ctx, to_table_cell, cell_out,
			      KITTY_PLACEHOLDER_CP);
	layout_append_utf8_cp(ctx, to_table_cell, cell_out,
			      kitty_num_to_diacritic[row]);
	layout_append_utf8_cp(ctx, to_table_cell, cell_out,
			      kitty_num_to_diacritic[col]);
}

static int parse_formula_marker(const char *raw, size_t raw_len, size_t pos,
				unsigned int *out_id, size_t *out_end)
{
	unsigned int id = 0;
	size_t i;
	int saw_digit = 0;

	if (!raw || pos + FORMULA_MARKER_LEN >= raw_len)
		return 0;
	if (memcmp(raw + pos, FORMULA_MARKER, FORMULA_MARKER_LEN) != 0)
		return 0;

	i = pos + FORMULA_MARKER_LEN;
	while (i < raw_len && raw[i] >= '0' && raw[i] <= '9') {
		id = id * 10u + (unsigned int)(raw[i] - '0');
		saw_digit = 1;
		i++;
	}
	if (!saw_digit || i >= raw_len || raw[i] != '\x1f')
		return 0;

	if (out_id)
		*out_id = id;
	if (out_end)
		*out_end = i + 1;
	return 1;
}

static int inline_raw_has_formula(const char *raw, size_t raw_len)
{
	size_t i = 0;

	while (i < raw_len) {
		if (raw[i] == FORMULA_MARKER[0] &&
		    parse_formula_marker(raw, raw_len, i, NULL, NULL))
			return 1;
		i++;
	}
	return 0;
}

static size_t inline_text_width(const char *raw, size_t raw_len)
{
	char *plain = NULL;
	size_t width = 0;

	strip_ansi(raw, raw_len, &plain);
	if (plain) {
		width = utf8_display_width(plain);
		free(plain);
	}
	return width;
}

static size_t inline_raw_natural_width(struct ansi_ctx *ctx, const char *raw,
				       size_t raw_len)
{
	size_t max_w = 0;
	size_t cur_w = 0;
	size_t seg_start = 0;
	size_t i = 0;

	while (i < raw_len) {
		unsigned int id;
		size_t end;

		if (raw[i] == '\n') {
			cur_w += inline_text_width(raw + seg_start,
						   i - seg_start);
			if (cur_w > max_w)
				max_w = cur_w;
			cur_w = 0;
			i++;
			seg_start = i;
			continue;
		}

		if (parse_formula_marker(raw, raw_len, i, &id, &end)) {
			struct formula_atom *atom;

			cur_w += inline_text_width(raw + seg_start,
						   i - seg_start);
			atom = formula_lookup(ctx, id);
			if (atom)
				cur_w += atom->cols;
			i = end;
			seg_start = i;
			continue;
		}
		i++;
	}

	cur_w += inline_text_width(raw + seg_start, raw_len - seg_start);
	if (cur_w > max_w)
		max_w = cur_w;
	return max_w;
}

static int inline_push_run(morph_array_t *line, struct inline_run *run)
{
	struct inline_run *slot = morph_array_push(line);

	if (!slot)
		return -ENOMEM;
	*slot = *run;
	return 0;
}

static void inline_emit_formula_row(struct ansi_ctx *ctx,
				    struct formula_atom *atom,
				    unsigned int row,
				    int to_table_cell,
				    morph_buf_t *cell_out)
{
	char sgr[64];
	int n;

	if (!atom)
		return;

	if (!atom->emitted) {
		layout_append(ctx, to_table_cell, cell_out, atom->transfer,
			      atom->transfer_len);
		atom->emitted = 1;
	}

	if (!atom->use_placeholder) {
		layout_append_spaces(ctx, to_table_cell, cell_out, atom->cols);
		return;
	}

	n = snprintf(sgr, sizeof(sgr), "\033[38;2;%u;%u;%um",
		     (atom->image_id >> 16) & 0xffu,
		     (atom->image_id >> 8) & 0xffu,
		     atom->image_id & 0xffu);
	if (n < 0 || (size_t)n >= sizeof(sgr)) {
		layout_append_spaces(ctx, to_table_cell, cell_out, atom->cols);
		return;
	}

	layout_append(ctx, to_table_cell, cell_out, sgr, (size_t)n);
	for (unsigned int col = 0; col < atom->cols; col++)
		layout_append_kitty_placeholder_cell(ctx, to_table_cell,
						     cell_out, row, col);
	layout_append_cstr(ctx, to_table_cell, cell_out, "\033[39m");
}

static void inline_flush_line(struct ansi_ctx *ctx, morph_array_t *line,
			      int to_table_cell, morph_buf_t *cell_out)
{
	struct inline_run *runs = line->elts;
	unsigned int baseline = 0;
	unsigned int descent = 0;
	unsigned int height;

	if (line->nelts == 0)
		return;

	for (size_t i = 0; i < line->nelts; i++) {
		unsigned int run_descent;

		if (runs[i].baseline > baseline)
			baseline = runs[i].baseline;
		run_descent = runs[i].rows > runs[i].baseline ?
			      runs[i].rows - runs[i].baseline - 1u : 0u;
		if (run_descent > descent)
			descent = run_descent;
	}
	height = baseline + 1u + descent;

	for (unsigned int y = 0; y < height; y++) {
		if (y > 0)
			layout_newline(ctx, to_table_cell, cell_out);
		for (size_t i = 0; i < line->nelts; i++) {
			unsigned int run_y = baseline - runs[i].baseline;

			if (!runs[i].is_formula) {
				if (y == run_y) {
					layout_append(ctx, to_table_cell,
						      cell_out, runs[i].raw,
						      runs[i].raw_len);
				} else {
					layout_append_spaces(ctx, to_table_cell,
							     cell_out,
							     runs[i].cols);
				}
				continue;
			}

			if (y >= run_y && y < run_y + runs[i].rows) {
				inline_emit_formula_row(ctx, runs[i].atom,
							y - run_y,
							to_table_cell,
							cell_out);
			} else {
				layout_append_spaces(ctx, to_table_cell,
						     cell_out, runs[i].cols);
			}
		}
	}

	morph_array_clear(line);
}

static void inline_add_run(struct ansi_ctx *ctx, morph_array_t *line,
			   struct inline_run *run, size_t *line_cols,
			   size_t max_cols, int to_table_cell,
			   morph_buf_t *cell_out, int *emitted_any)
{
	if (max_cols == 0)
		max_cols = 1;
	if (run->cols > 0 && *line_cols > 0 &&
	    *line_cols + run->cols > max_cols) {
		inline_flush_line(ctx, line, to_table_cell, cell_out);
		layout_newline(ctx, to_table_cell, cell_out);
		*line_cols = 0;
		*emitted_any = 1;
	}
	if (inline_push_run(line, run) != 0)
		return;
	*line_cols += run->cols;
}

static void render_inline_layout(struct ansi_ctx *ctx, const char *raw,
				 size_t raw_len, size_t max_cols,
				 int to_table_cell, morph_buf_t *cell_out)
{
	morph_array_t line;
	size_t seg_start = 0;
	size_t line_cols = 0;
	size_t i = 0;
	int emitted_any = 0;

	if (!inline_raw_has_formula(raw, raw_len)) {
		layout_append(ctx, to_table_cell, cell_out, raw, raw_len);
		return;
	}

	if (morph_array_init(&line, 8, sizeof(struct inline_run)) != 0) {
		layout_append(ctx, to_table_cell, cell_out, raw, raw_len);
		return;
	}

	while (i < raw_len) {
		unsigned int id;
		size_t end;

		if (raw[i] == '\n') {
			struct inline_run run;
			size_t seg_len = i - seg_start;

			if (seg_len > 0) {
				memset(&run, 0, sizeof(run));
				run.raw = raw + seg_start;
				run.raw_len = seg_len;
				run.cols = inline_text_width(run.raw,
							     run.raw_len);
				run.rows = 1;
				run.baseline = 0;
				inline_add_run(ctx, &line, &run, &line_cols,
					       max_cols, to_table_cell,
					       cell_out, &emitted_any);
			}
			inline_flush_line(ctx, &line, to_table_cell, cell_out);
			layout_newline(ctx, to_table_cell, cell_out);
			emitted_any = 1;
			line_cols = 0;
			i++;
			seg_start = i;
			continue;
		}

		if (parse_formula_marker(raw, raw_len, i, &id, &end)) {
			struct formula_atom *atom;
			struct inline_run run;
			size_t seg_len = i - seg_start;

			if (seg_len > 0) {
				memset(&run, 0, sizeof(run));
				run.raw = raw + seg_start;
				run.raw_len = seg_len;
				run.cols = inline_text_width(run.raw,
							     run.raw_len);
				run.rows = 1;
				run.baseline = 0;
				inline_add_run(ctx, &line, &run, &line_cols,
					       max_cols, to_table_cell,
					       cell_out, &emitted_any);
			}

			atom = formula_lookup(ctx, id);
			if (atom) {
				memset(&run, 0, sizeof(run));
				run.is_formula = 1;
				run.atom = atom;
				run.cols = atom->cols;
				run.rows = atom->rows > 0 ? atom->rows : 1;
				run.baseline = run.rows / 2u;
				inline_add_run(ctx, &line, &run, &line_cols,
					       max_cols, to_table_cell,
					       cell_out, &emitted_any);
			}
			i = end;
			seg_start = i;
			continue;
		}
		i++;
	}

	if (seg_start < raw_len) {
		struct inline_run run;

		memset(&run, 0, sizeof(run));
		run.raw = raw + seg_start;
		run.raw_len = raw_len - seg_start;
		run.cols = inline_text_width(run.raw, run.raw_len);
		run.rows = 1;
		run.baseline = 0;
		inline_add_run(ctx, &line, &run, &line_cols, max_cols,
			       to_table_cell, cell_out, &emitted_any);
	}

	if (line.nelts > 0)
		inline_flush_line(ctx, &line, to_table_cell, cell_out);
	else if (!emitted_any)
		layout_append(ctx, to_table_cell, cell_out, raw, raw_len);

	morph_array_cleanup(&line);
}

static void append_mathjax_cursor_advance(struct ansi_ctx *ctx,
                                          unsigned int cols,
                                          unsigned int rows,
                                          int display)
{
        if (!ctx || cols == 0 || rows == 0)
                return;

        if (!display) {
                append_spaces(ctx, cols);
                return;
        }

        for (unsigned int r = 0; r < rows; r++) {
                append_spaces(ctx, cols);
                if (r + 1 < rows) {
                        if (ctx->table && ctx->table->cell_active)
                                out_append(ctx, "\n");
                        else
                                newline_with_prefix(ctx);
                }
        }
        if (ctx->table && ctx->table->cell_active)
                out_append(ctx, "\n");
        else
                newline_with_prefix(ctx);
}

static int render_mathjax_kitty(struct ansi_ctx *ctx, const char *latex,
                                size_t len, int display)
{
        mjx_opts opts;
        mjx_ctx *mjx = NULL;
        mjx_buf *buf = NULL;
        const uint32_t *pixels;
        char *expr = NULL;
        size_t bytes;
        size_t offset = 0;
        const size_t chunk_size = 3000;
        unsigned int width;
        unsigned int height;
        unsigned int cols;
        unsigned int rows;
        unsigned int cell_w;
        unsigned int cell_h;
        unsigned int image_id = 0;
        int use_placeholder = 0;
        int collecting_inline;
        int block_layout = display;
        int rc = -1;

        if (!ctx || !latex || len == 0)
                return -1;

        expr = malloc(len + 1);
        if (!expr)
                return -1;
        memcpy(expr, latex, len);
        expr[len] = '\0';

        get_term_cell_size(&cell_w, &cell_h);

        memset(&opts, 0, sizeof(opts));
        opts.font_path = MORPH_MATHJAX_FONT_PATH;
        if (display) {
                opts.font_size = mathjax_font_size_from_cell(
                        cell_h, MATHJAX_DISPLAY_DEFAULT_SCALE,
                        MATHJAX_DISPLAY_MIN_SIZE,
                        "MORPH_MATH_DISPLAY_SCALE",
                        "MORPH_MATH_DISPLAY_MAX_SIZE");
        } else {
                opts.font_size = mathjax_font_size_from_cell(
                        cell_h, MATHJAX_INLINE_DEFAULT_SCALE,
                        MATHJAX_INLINE_MIN_SIZE,
                        "MORPH_MATH_INLINE_SCALE",
                        "MORPH_MATH_INLINE_MAX_SIZE");
        }
        opts.fg_color = MATHJAX_FG_COLOR;
        opts.bg_color = MATHJAX_BG_COLOR;
        opts.dpi = MATHJAX_DPI;

        mjx = mjx_init(&opts);
        if (!mjx)
                goto out;

        buf = mjx_render_latex(mjx, expr,
                        display ? MJX_STYLE_DISPLAY : MJX_STYLE_INLINE);
        if (!buf)
                goto out;

        pixels = mjx_buf_pixels(buf);
        width = mjx_buf_width(buf);
        height = mjx_buf_height(buf);

        cols = mathjax_cell_count(width, cell_w);
        rows = mathjax_cell_count(height, cell_h);
        if (!display && rows > 1)
                block_layout = 1;
        if (ctx->use_kitty_placeholders) {
                image_id = ctx->next_image_id++;
                if (ctx->next_image_id == 0 || ctx->next_image_id > 0xffffffu)
                        ctx->next_image_id = 1;
                use_placeholder =
                        kitty_placeholder_supported(cols, rows, image_id);
        }

        bytes = (size_t)width * height * 4;
        if (!pixels || bytes == 0)
                goto out;

        collecting_inline = ctx->inline_active ||
                             (ctx->table && ctx->table->cell_active);
        if (collecting_inline) {
                char *transfer;
                size_t transfer_len = 0;
                unsigned int formula_id;

                transfer = build_formula_transfer(pixels, bytes, width,
                                height, cols, rows, image_id,
                                use_placeholder, &transfer_len);
                if (!transfer)
                        goto out;
                if (store_formula_atom(ctx, transfer, transfer_len, cols,
                                rows, image_id, use_placeholder,
                                &formula_id) != 0) {
                        free(transfer);
                        goto out;
                }
                append_formula_marker(ctx, formula_id);
                rc = 0;
                goto out;
        }

        if (block_layout) {
                if (ctx->table && ctx->table->cell_active)
                        out_append(ctx, "\n");
                else
                        newline_with_prefix(ctx);
        }

        while (offset < bytes) {
                size_t remaining = bytes - offset;
                size_t n = remaining < chunk_size ? remaining : chunk_size;
                char header[96];
                int header_len;

                if (offset == 0) {
                        if (use_placeholder) {
                                header_len = snprintf(header, sizeof(header),
                                        "\033_Ga=T,f=32,s=%u,v=%u,i=%u,U=1,c=%u,r=%u,q=2,m=%d;",
                                        width, height, image_id, cols, rows,
                                        offset + n < bytes ? 1 : 0);
                        } else {
                                header_len = snprintf(header, sizeof(header),
                                        "\033_Ga=T,f=32,s=%u,v=%u,c=%u,r=%u,C=1,z=0,m=%d;",
                                        width, height, cols, rows,
                                        offset + n < bytes ? 1 : 0);
                        }
                } else {
                        header_len = snprintf(header, sizeof(header),
                                "\033_Gm=%d;", offset + n < bytes ? 1 : 0);
                }
                if (header_len < 0 ||
                    (size_t)header_len >= sizeof(header))
                        goto out;

                out_append_n(ctx, header, (size_t)header_len);
                append_mathjax_b64(ctx, pixels, offset, n);
                out_append(ctx, "\033\\");
                offset += n;
        }

        if (use_placeholder)
                append_kitty_placeholder_grid(ctx, image_id, cols, rows,
                                              block_layout);
        else
                append_mathjax_cursor_advance(ctx, cols, rows, block_layout);
        rc = 0;

out:
        if (buf)
                mjx_buf_free(buf);
        if (mjx)
                mjx_free(mjx);
        free(expr);
        return rc;
}

static int enter_span(MD_SPANTYPE type, void *detail, void *userdata)
{
	struct ansi_ctx *ctx = userdata;
	switch (type) {
	case MD_SPAN_STRONG:
		ctx->bold_depth++;
		out_append(ctx, ANSI_BOLD);
		break;
	case MD_SPAN_EM:
		ctx->italic_depth++;
		out_append(ctx, ANSI_ITALIC);
		break;
	case MD_SPAN_U:
		ctx->underline_depth++;
		out_append(ctx, ANSI_UNDERLINE);
		break;
	case MD_SPAN_DEL:
		ctx->strike_depth++;
		out_append(ctx, ANSI_STRIKE);
		break;
	case MD_SPAN_CODE:
		ctx->code_depth++;
		out_append(ctx, ANSI_CYAN);
		break;
	case MD_SPAN_A: {
		struct MD_SPAN_A_DETAIL *a = detail;
		ctx->link_depth++;
		out_append(ctx, ANSI_UNDERLINE);
		out_append(ctx, ANSI_BLUE);
		/* Stash href to be appended after the link text. */
		ctx->current_link_href_pending = 1;
		ctx->link_href.len = 0;
		if (a && a->href.text) {
			char *hb = malloc(a->href.size + 1);
			if (hb) {
				memcpy(hb, a->href.text, a->href.size);
				hb[a->href.size] = '\0';
				ctx->link_href.buf = hb;
				ctx->link_href.cap = a->href.size + 1;
				ctx->link_href.len = a->href.size;
			}
		}
		break;
	}
	case MD_SPAN_IMG: {
		struct MD_SPAN_IMG_DETAIL *im = detail;
		out_append(ctx, ANSI_DIM);
		out_append(ctx, "[image: ");
		(void)im;
		break;
	}
	case MD_SPAN_LATEXMATH:
		ctx->latex_display = 0;
		break;
	case MD_SPAN_LATEXMATH_DISPLAY:
		ctx->latex_display = 1;
		break;
	case MD_SPAN_WIKILINK:
		out_append(ctx, ANSI_UNDERLINE);
		out_append(ctx, ANSI_BLUE);
		break;
	default:
		break;
	}
	return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *userdata)
{
	(void)detail;
	struct ansi_ctx *ctx = userdata;
	switch (type) {
	case MD_SPAN_STRONG:
		ctx->bold_depth--;
		out_append(ctx, ANSI_RESET);
		reapply_inline(ctx);
		break;
	case MD_SPAN_EM:
		ctx->italic_depth--;
		out_append(ctx, ANSI_RESET);
		reapply_inline(ctx);
		break;
	case MD_SPAN_U:
		ctx->underline_depth--;
		out_append(ctx, ANSI_RESET);
		reapply_inline(ctx);
		break;
	case MD_SPAN_DEL:
		ctx->strike_depth--;
		out_append(ctx, ANSI_RESET);
		reapply_inline(ctx);
		break;
	case MD_SPAN_CODE:
		ctx->code_depth--;
		out_append(ctx, ANSI_RESET);
		reapply_inline(ctx);
		break;
	case MD_SPAN_A:
		out_append(ctx, ANSI_RESET);
		ctx->link_depth--;
		if (ctx->current_link_href_pending && ctx->link_href.buf &&
		    ctx->link_href.len > 0) {
			if (is_video_ext(ctx->link_href.buf, ctx->link_href.len)) {
				collect_media(ctx, ctx->link_href.buf, ctx->link_href.len, "video");
			}
			out_append(ctx, ANSI_DIM);
			out_append(ctx, " (");
			out_append_n(ctx, ctx->link_href.buf, ctx->link_href.len);
			out_append(ctx, ")");
			out_append(ctx, ANSI_RESET);
		}
		ctx->current_link_href_pending = 0;
		free(ctx->link_href.buf);
		ctx->link_href.buf = NULL;
		ctx->link_href.cap = 0;
		ctx->link_href.len = 0;
		reapply_inline(ctx);
		break;
	case MD_SPAN_IMG: {
		struct MD_SPAN_IMG_DETAIL *im = detail;
		out_append(ctx, "]");
		if (im && im->src.text && im->src.size > 0) {
			out_append(ctx, "(");
			append_attr(ctx, &im->src);
			out_append(ctx, ")");
			collect_media(ctx, im->src.text, im->src.size, "image");
		}
		out_append(ctx, ANSI_RESET);
		reapply_inline(ctx);
		break;
	}
	case MD_SPAN_LATEXMATH:
	case MD_SPAN_LATEXMATH_DISPLAY:
		out_append(ctx, ANSI_RESET);
		reapply_inline(ctx);
		break;
	case MD_SPAN_WIKILINK:
		out_append(ctx, ANSI_RESET);
		reapply_inline(ctx);
		break;
	default:
		break;
	}
	return 0;
}

/* ---------------- text ---------------- */
static int text_callback(MD_TEXTTYPE type, const MD_CHAR *text,
			 MD_SIZE size, void *userdata)
{
	struct ansi_ctx *ctx = userdata;
	switch (type) {
	case MD_TEXT_NULLCHAR:
		out_append(ctx, "\xEF\xBF\xBD"); /* U+FFFD */
		return 0;
	case MD_TEXT_BR:
		newline_with_prefix(ctx);
		return 0;
	case MD_TEXT_SOFTBR:
		out_append(ctx, " ");
		return 0;
	case MD_TEXT_NORMAL:
	case MD_TEXT_ENTITY:
	case MD_TEXT_HTML:
		out_append_n(ctx, text, size);
		return 0;
	case MD_TEXT_LATEXMATH: {
                if (render_mathjax_kitty(ctx, text, size,
                                         ctx->latex_display) != 0)
			out_append_n(ctx, text, size);
		break;
	}
	case MD_TEXT_CODE: {
		if (ctx->in_code_block) {
			morph_buf_append(&ctx->code_raw, text, size);
		} else if (ctx->code_depth > 0 && ctx->table == NULL) {
			MD_SIZE start = 0;
			for (MD_SIZE i = 0; i < size; i++) {
				if (text[i] == '\n') {
					out_append_n(ctx, text + start, i - start);
					out_append(ctx, ANSI_RESET);
					newline_with_prefix(ctx);
					start = i + 1;
				}
			}
			if (start < size)
				out_append_n(ctx, text + start, size - start);
		} else {
			out_append_n(ctx, text, size);
		}
		return 0;
	}
	default:
		out_append_n(ctx, text, size);
		return 0;
	}
	return 0;
}

static void debug_log(const char *msg, void *userdata)
{
	(void)userdata;
	(void)msg;
}

static void free_media(struct ansi_ctx *ctx)
{
	for (int i = 0; i < ctx->media_count; i++) {
		free(ctx->media[i].type);
		free(ctx->media[i].path);
	}
}

/* ---------------- markdown pre-normalization ---------------- */

static int md_decode_cp(const char *s, size_t len, size_t off,
			unsigned *cp, size_t *cp_len)
{
	if (!s || off >= len || !cp || !cp_len)
		return 0;
	return utf8_decode_codepoint(s + off, len - off, cp, cp_len);
}

static int is_table_sep_line(const char *line, size_t len)
{
	int has_pipe = 0;
	int has_dash = 0;
	size_t i = 0;
	while (i < len) {
		unsigned char c = (unsigned char)line[i];
		unsigned cp;
		size_t cp_len;

		if (c == '|') {
			has_pipe = 1;
			i++;
		} else if (c == '-') {
			has_dash = 1;
			i++;
		} else if (c == ':' || c == ' ' || c == '\t') {
			i++;
		} else if (md_decode_cp(line, len, i, &cp, &cp_len)) {
			if (utf8_is_fullwidth_pipe_cp(cp)) {
				has_pipe = 1;
				i += cp_len;
			} else if (utf8_is_fullwidth_dash_cp(cp)) {
				has_dash = 1;
				i += cp_len;
			} else if (utf8_is_fullwidth_colon_cp(cp)) {
				i += cp_len;
			} else {
				return 0;
			}
		} else {
			return 0;
		}
	}
	return has_pipe && has_dash;
}

static int line_has_pipe(const char *line, size_t len)
{
	size_t i = 0;
	while (i < len) {
		unsigned char c = (unsigned char)line[i];
		unsigned cp;
		size_t cp_len;

		if (c == '|')
			return 1;
		if (md_decode_cp(line, len, i, &cp, &cp_len)) {
			if (utf8_is_fullwidth_pipe_cp(cp))
				return 1;
			i += cp_len;
		} else {
			i++;
		}
	}
	return 0;
}

static char *md_normalize(const char *md)
{
	size_t len = strlen(md);
	morph_buf_t out;
	int rc = morph_buf_init(&out, len + 1);
	if (rc != 0)
		return NULL;

	const char *p = md;
	int in_code_fence = 0;
	char fence_char = 0;
	int fence_len = 0;

	char *pending = NULL;
	size_t pending_len = 0;
	int pending_blank = 0;
	int prev_was_blank = 1;

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t line_len;
		const char *next_line;
		if (nl) {
			const char *raw_end = nl;
			while (raw_end > p && raw_end[-1] == '\r')
				raw_end--;
			line_len = (size_t)(raw_end - p);
			next_line = nl + 1;
		} else {
			line_len = strlen(p);
			next_line = p + line_len;
		}

		while (line_len > 0 && (p[line_len - 1] == '\r' ||
		       p[line_len - 1] == ' ' || p[line_len - 1] == '\t'))
			line_len--;

		int is_blank = (line_len == 0);

		if (line_len >= 3 && (p[0] == '`' || p[0] == '~')) {
			char fc = p[0];
			int fl = 0;
			size_t fi = 0;
			while (fi < line_len && p[fi] == fc) {
				fl++;
				fi++;
			}
			if (fl >= 3) {
				if (!in_code_fence) {
					in_code_fence = 1;
					fence_char = fc;
					fence_len = fl;
				} else if (fc == fence_char && fl >= fence_len) {
					in_code_fence = 0;
				}
			}
		}

		if (pending) {
			int need_blank_before_pending = 0;
			if (!in_code_fence && !is_blank && !pending_blank &&
			    is_table_sep_line(p, line_len) &&
			    line_has_pipe(pending, pending_len)) {
				need_blank_before_pending = !prev_was_blank;
			}

			if (need_blank_before_pending) {
				rc = morph_buf_putc(&out, '\n');
				if (rc != 0) { free(pending); goto fail; }
			}

			rc = morph_buf_append(&out, pending, pending_len);
			if (rc != 0) { free(pending); goto fail; }
			rc = morph_buf_putc(&out, '\n');
			if (rc != 0) { free(pending); goto fail; }

			prev_was_blank = pending_blank;
			free(pending);
			pending = NULL;
		}

		if (is_blank) {
			rc = morph_buf_putc(&out, '\n');
			if (rc != 0) { free(pending); goto fail; }
			prev_was_blank = 1;
		} else {
			pending_len = line_len;
			pending = malloc(pending_len + 1);
			if (!pending) goto fail;

			size_t wi = 0;
			for (size_t i = 0; i < line_len; ) {
				unsigned cp;
				size_t cp_len;

				if (md_decode_cp(p, line_len, i, &cp, &cp_len)) {
					if (utf8_is_fullwidth_pipe_cp(cp)) {
						pending[wi++] = '|';
						i += cp_len;
						continue;
					}
					if (is_table_sep_line(p, line_len)) {
						if (utf8_is_fullwidth_dash_cp(cp)) {
							pending[wi++] = '-';
							i += cp_len;
							continue;
						}
						if (utf8_is_fullwidth_colon_cp(cp)) {
							pending[wi++] = ':';
							i += cp_len;
							continue;
						}
					}
				}
				pending[wi++] = p[i];
				i++;
			}
			pending_len = wi;
			pending[wi] = '\0';
			pending_blank = 0;
		}

		p = next_line;
	}

	if (pending) {
		rc = morph_buf_append(&out, pending, pending_len);
		if (rc != 0) { free(pending); goto fail; }
		rc = morph_buf_putc(&out, '\n');
		if (rc != 0) { free(pending); goto fail; }
		free(pending);
	}

	return morph_buf_detach(&out);

fail:
	morph_buf_cleanup(&out);
	return NULL;
}

/* ---------------- public API ---------------- */
size_t markdown_render_ansi_to_buf(const char *md, char *buf, size_t buf_len)
{
	if (!md)
		return 0;
	if (buf && buf_len > 0)
		buf[0] = '\0';

	char *norm = md_normalize(md);
	const char *input = norm ? norm : md;

	struct ansi_ctx ctx = {0};
	sbuf_init(&ctx.out, buf, buf_len);
	ctx.term_width = get_term_width();
	ctx.use_kitty_placeholders = detect_kitty_placeholders();
	ctx.next_image_id = 1;

	MD_PARSER parser = {0};
	parser.abi_version = 0;
	parser.flags = MD_FLAG_STRIKETHROUGH | MD_FLAG_TABLES |
		       MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS |
		       MD_FLAG_COLLAPSEWHITESPACE | MD_FLAG_LATEXMATHSPANS;
	parser.enter_block = enter_block;
	parser.leave_block = leave_block;
	parser.enter_span = enter_span;
	parser.leave_span = leave_span;
	parser.text = text_callback;
	parser.debug_log = debug_log;

	md_parse(input, (MD_SIZE)strlen(input), &parser, &ctx);

	free(norm);
	free(ctx.link_href.buf);
	morph_buf_cleanup(&ctx.code_raw);
	morph_buf_cleanup(&ctx.inline_raw);
	free_formulas(&ctx);
	if (ctx.table)
		free_table(ctx.table);
	free_media(&ctx);

	if (buf && ctx.out.len < ctx.out.cap)
		buf[ctx.out.len] = '\0';
	return ctx.out.len;
}

static void render_ansi_impl(const char *md, struct ansi_ctx *ctx)
{
	if (!md)
		return;

	char *norm = md_normalize(md);
	const char *input = norm ? norm : md;

	MD_PARSER parser = {0};
	parser.abi_version = 0;
	parser.flags = MD_FLAG_STRIKETHROUGH | MD_FLAG_TABLES |
		       MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS |
		       MD_FLAG_COLLAPSEWHITESPACE | MD_FLAG_LATEXMATHSPANS;
	parser.enter_block = enter_block;
	parser.leave_block = leave_block;
	parser.enter_span = enter_span;
	parser.leave_span = leave_span;
	parser.text = text_callback;
	parser.debug_log = debug_log;

	md_parse(input, (MD_SIZE)strlen(input), &parser, ctx);

	free(norm);
	free(ctx->link_href.buf);
	morph_buf_cleanup(&ctx->code_raw);
	morph_buf_cleanup(&ctx->inline_raw);
	free_formulas(ctx);
	if (ctx->table)
		free_table(ctx->table);
}

void markdown_render_ansi(const char *md)
{
	if (!md)
		return;
	size_t len = markdown_render_ansi_to_buf(md, NULL, 0);
	size_t buf_len = len + 1;
	char *buf = malloc(buf_len);
	if (!buf)
		return;

	struct ansi_ctx ctx = {0};
	sbuf_init(&ctx.out, buf, buf_len);
	ctx.term_width = get_term_width();
	ctx.use_kitty_placeholders = detect_kitty_placeholders();
	ctx.next_image_id = 1;

	render_ansi_impl(md, &ctx);

	if (ctx.out.len < ctx.out.cap)
		buf[ctx.out.len] = '\0';
	printf("%s\n", buf);
	fflush(stdout);
	free(buf);
	free_media(&ctx);
}

void markdown_render_ansi_with_media(const char *md, markdown_media_cb cb, void *user)
{
	if (!md || !cb)
		return;
	size_t len = markdown_render_ansi_to_buf(md, NULL, 0);
	size_t buf_len = len + 1;
	char *buf = malloc(buf_len);
	if (!buf)
		return;

	struct ansi_ctx ctx = {0};
	sbuf_init(&ctx.out, buf, buf_len);
	ctx.term_width = get_term_width();
	ctx.use_kitty_placeholders = detect_kitty_placeholders();
	ctx.next_image_id = 1;

	render_ansi_impl(md, &ctx);

	if (ctx.out.len < ctx.out.cap)
		buf[ctx.out.len] = '\0';
	printf("%s\n", buf);
	fflush(stdout);
	free(buf);

	for (int i = 0; i < ctx.media_count; i++) {
		cb(ctx.media[i].type, ctx.media[i].path, user);
	}
	free_media(&ctx);
}
