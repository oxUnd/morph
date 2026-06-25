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
#define MATHJAX_INLINE_MIN_SIZE 14.0
#define MATHJAX_INLINE_MAX_SIZE 22.0
#define MATHJAX_DISPLAY_MIN_SIZE 24.0
#define MATHJAX_DISPLAY_MAX_SIZE 44.0

/* ---------------- text buffer ---------------- */
/* sbuf is defined in highlight.h */

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

static double clamp_double(double v, double lo, double hi)
{
        if (v < lo)
                return lo;
        if (v > hi)
                return hi;
        return v;
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
		if ((unsigned char)raw[ri] == 0x1b && ri + 1 < raw_len && raw[ri + 1] == '[') {
			size_t end = ri + 2;
			while (end < raw_len && !((raw[end] >= 'A' && raw[end] <= 'Z') ||
						  (raw[end] >= 'a' && raw[end] <= 'z')))
				end++;
			if (end < raw_len)
				end++;
			ri = end;
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
			size_t w = utf8_display_width(row->cells[c].plain);
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
			wrap_lines[r][c] = wrap_cell_content(
				row->cells[c].raw, row->cells[c].raw_len,
				row->cells[c].plain, row->cells[c].plain_len,
				col_w[c], &wrap_counts[r][c]);
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

static unsigned int mathjax_cell_count(unsigned int pixels, unsigned int cell_px)
{
        unsigned int cells;

        if (cell_px == 0)
                return 1;
        cells = (pixels + cell_px - 1) / cell_px;
        return cells > 0 ? cells : 1;
}

static void append_mathjax_cursor_advance(struct ansi_ctx *ctx,
                                          unsigned int cols,
                                          unsigned int rows,
                                          int display)
{
        (void)ctx;
        (void)cols;
        (void)rows;
        (void)display;
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
                opts.font_size = clamp_double((double)cell_h * 1.6,
                                              MATHJAX_DISPLAY_MIN_SIZE,
                                              MATHJAX_DISPLAY_MAX_SIZE);
        } else {
                opts.font_size = clamp_double((double)cell_h * 0.95,
                                              MATHJAX_INLINE_MIN_SIZE,
                                              MATHJAX_INLINE_MAX_SIZE);
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

        bytes = (size_t)width * height * 4;
        if (!pixels || bytes == 0)
                goto out;

        if (block_layout)
                out_append(ctx, "\n");

        while (offset < bytes) {
                size_t remaining = bytes - offset;
                size_t n = remaining < chunk_size ? remaining : chunk_size;
                char header[96];
                int header_len;

                if (offset == 0) {
                        header_len = snprintf(header, sizeof(header),
                                "\033_Ga=T,f=32,s=%u,v=%u,c=%u,r=%u,m=%d;",
                                width, height, cols, rows,
                                offset + n < bytes ? 1 : 0);
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
