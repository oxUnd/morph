#include "markdown.h"
#include "md4c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define ANSI_BG_CODE   "\033[48;5;236m"

/* ---------------- text buffer ---------------- */
struct sbuf {
	char *buf;	/* may be NULL when only sizing */
	size_t len;
	size_t cap;	/* 0 when sizing only */
};

static void sbuf_init(struct sbuf *s, char *buf, size_t cap)
{
	s->buf = buf;
	s->cap = cap;
	s->len = 0;
}

static void sbuf_append_n(struct sbuf *s, const char *src, size_t n)
{
	if (!s->buf) {
		s->len += n;
		return;
	}
	size_t space = s->cap > 0 ? s->cap - 1 - s->len : 0;
	size_t to_write = n < space ? n : space;
	if (to_write > 0) {
		memcpy(s->buf + s->len, src, to_write);
		s->len += to_write;
	}
}

static void sbuf_append(struct sbuf *s, const char *str)
{
	sbuf_append_n(s, str, strlen(str));
}

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
	struct table_row *rows;
	unsigned row_count;
	unsigned row_cap;
	int in_head;
	unsigned head_row_count;
	/* current cell being built */
	char *cell_raw;
	size_t cell_raw_len;
	size_t cell_raw_cap;
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
	struct sbuf link_href;
	struct collected_media media[64];
	int media_count;
};

/* Append to either the active table cell or the main output, depending on
 * whether we are currently rendering a table cell. */
static void out_append_n(struct ansi_ctx *ctx, const char *src, size_t n)
{
	if (ctx->table && ctx->table->cell_raw) {
		struct table_state *t = ctx->table;
		if (t->cell_raw_len + n + 1 > t->cell_raw_cap) {
			size_t new_cap = t->cell_raw_cap ? t->cell_raw_cap * 2 : 64;
			while (new_cap < t->cell_raw_len + n + 1)
				new_cap *= 2;
			char *nb = realloc(t->cell_raw, new_cap);
			if (!nb)
				return;
			t->cell_raw = nb;
			t->cell_raw_cap = new_cap;
		}
		memcpy(t->cell_raw + t->cell_raw_len, src, n);
		t->cell_raw_len += n;
		t->cell_raw[t->cell_raw_len] = '\0';
		return;
	}
	sbuf_append_n(&ctx->out, src, n);
}

static void out_append(struct ansi_ctx *ctx, const char *str)
{
	out_append_n(ctx, str, strlen(str));
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
		out_append(ctx, ANSI_DIM);
		out_append(ctx, "```");
		if (c->lang.text && c->lang.size > 0) {
			out_append_n(ctx, c->lang.text, c->lang.size);
		}
		out_append(ctx, ANSI_RESET);
		newline_with_prefix(ctx);
		out_append(ctx, ANSI_BG_CODE);
		out_append(ctx, ANSI_GRAY);
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
		if (t->row_count == t->row_cap) {
			size_t new_cap = t->row_cap ? t->row_cap * 2 : 4;
			struct table_row *nr = realloc(t->rows, new_cap * sizeof(*nr));
			if (!nr)
				break;
			t->rows = nr;
			t->row_cap = (unsigned)new_cap;
		}
		t->rows[t->row_count].cells = calloc(t->col_count, sizeof(struct table_cell));
		t->rows[t->row_count].cell_count = 0;
		t->row_count++;
		break;
	}
	case MD_BLOCK_TH:
	case MD_BLOCK_TD: {
		struct table_state *t = ctx->table;
		if (!t)
			break;
		struct MD_BLOCK_TD_DETAIL *cd = detail;
		struct table_row *row = &t->rows[t->row_count - 1];
		if (row->cell_count < t->col_count && t->in_head)
			t->align[row->cell_count] = cd->align;
		t->cell_raw = malloc(64);
		t->cell_raw_cap = 64;
		t->cell_raw_len = 0;
		if (t->cell_raw)
			t->cell_raw[0] = '\0';
		break;
	}
	default:
		break;
	}
	return 0;
}

/* Strip ANSI escape sequences and count visible width (assumes ASCII +
 * UTF-8). Returns visible byte count; for now we approximate width as the
 * count of UTF-8 character starts (good enough for ASCII-heavy tables). */
static size_t strip_ansi(const char *src, size_t len, char **out_plain)
{
	char *plain = malloc(len + 1);
	if (!plain) {
		*out_plain = NULL;
		return 0;
	}
	size_t j = 0;
	for (size_t i = 0; i < len; ) {
		if (src[i] == '\033' && i + 1 < len && src[i + 1] == '[') {
			i += 2;
			while (i < len && !((src[i] >= 'A' && src[i] <= 'Z') ||
					    (src[i] >= 'a' && src[i] <= 'z')))
				i++;
			if (i < len) i++;
			continue;
		}
		plain[j++] = src[i++];
	}
	plain[j] = '\0';
	*out_plain = plain;
	return j;
}

static size_t utf8_visible_cols(const char *s, size_t len)
{
	size_t cols = 0;
	for (size_t i = 0; i < len; ) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x80) { i++; cols++; }
		else if ((c & 0xE0) == 0xC0) { i += 2; cols++; }
		else if ((c & 0xF0) == 0xE0) { i += 3; cols += 2; /* CJK approx */ }
		else if ((c & 0xF8) == 0xF0) { i += 4; cols += 2; }
		else { i++; cols++; }
	}
	return cols;
}

static void render_table(struct ansi_ctx *ctx, struct table_state *t)
{
	if (!t || t->col_count == 0 || t->row_count == 0)
		return;

	size_t *col_w = calloc(t->col_count, sizeof(size_t));
	if (!col_w)
		return;

	for (unsigned r = 0; r < t->row_count; r++) {
		struct table_row *row = &t->rows[r];
		for (unsigned c = 0; c < row->cell_count && c < t->col_count; c++) {
			size_t w = utf8_visible_cols(row->cells[c].plain,
						     row->cells[c].plain_len);
			if (w > col_w[c])
				col_w[c] = w;
		}
	}

	for (unsigned r = 0; r < t->row_count; r++) {
		if (r > 0)
			newline_with_prefix(ctx);
		else
			start_content_line(ctx);
		struct table_row *row = &t->rows[r];
		for (unsigned c = 0; c < t->col_count; c++) {
			out_append(ctx, ANSI_DIM);
			out_append(ctx, "│ ");
			out_append(ctx, ANSI_RESET);
			reapply_inline(ctx);
			const char *raw = c < row->cell_count ? row->cells[c].raw : "";
			size_t raw_len = c < row->cell_count ? row->cells[c].raw_len : 0;
			size_t plain_w = c < row->cell_count
				? utf8_visible_cols(row->cells[c].plain, row->cells[c].plain_len)
				: 0;
			size_t pad = col_w[c] > plain_w ? col_w[c] - plain_w : 0;
			MD_ALIGN al = t->align[c];
			size_t left = 0, right = 0;
			if (al == MD_ALIGN_RIGHT) left = pad;
			else if (al == MD_ALIGN_CENTER) { left = pad / 2; right = pad - left; }
			else right = pad;
			for (size_t i = 0; i < left; i++) out_append(ctx, " ");
			if (r == 0)
				out_append(ctx, ANSI_BOLD);
			out_append_n(ctx, raw, raw_len);
			if (r == 0) {
				out_append(ctx, ANSI_RESET);
				reapply_inline(ctx);
			}
			for (size_t i = 0; i < right; i++) out_append(ctx, " ");
			out_append(ctx, " ");
		}
		out_append(ctx, ANSI_DIM);
		out_append(ctx, "│");
		out_append(ctx, ANSI_RESET);
		reapply_inline(ctx);

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
	free(col_w);
	ctx->last_block_was_visible = 1;
}

static void free_table(struct table_state *t)
{
	if (!t)
		return;
	for (unsigned r = 0; r < t->row_count; r++) {
		for (unsigned c = 0; c < t->rows[r].cell_count; c++) {
			free(t->rows[r].cells[c].raw);
			free(t->rows[r].cells[c].plain);
		}
		free(t->rows[r].cells);
	}
	free(t->rows);
	free(t->align);
	free(t->cell_raw);
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
	case MD_BLOCK_CODE:
		out_append(ctx, ANSI_RESET);
		newline_with_prefix(ctx);
		out_append(ctx, ANSI_DIM);
		out_append(ctx, "```");
		out_append(ctx, ANSI_RESET);
		ctx->code_depth--;
		ctx->last_block_was_visible = 1;
		break;
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
		if (!t || !t->cell_raw)
			break;
		struct table_row *row = &t->rows[t->row_count - 1];
		if (row->cell_count < t->col_count) {
			struct table_cell *cell = &row->cells[row->cell_count];
			cell->raw = t->cell_raw;
			cell->raw_len = t->cell_raw_len;
			cell->plain_len = strip_ansi(cell->raw, cell->raw_len,
						     &cell->plain);
			row->cell_count++;
		} else {
			free(t->cell_raw);
		}
		t->cell_raw = NULL;
		t->cell_raw_len = 0;
		t->cell_raw_cap = 0;
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
		out_append(ctx, ANSI_BG_CODE);
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
	case MD_SPAN_LATEXMATH_DISPLAY:
		out_append(ctx, ANSI_CYAN);
		out_append(ctx, "$");
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
		out_append(ctx, "$");
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
	case MD_TEXT_LATEXMATH:
		out_append_n(ctx, text, size);
		return 0;
	case MD_TEXT_CODE: {
		/* For a fenced code block md4c emits text including '\n'.
		 * We must re-emit the line prefix (and re-open the bg color)
		 * after each newline so the colored block stays continuous. */
		if (ctx->code_depth > 0 && ctx->table == NULL) {
			MD_SIZE start = 0;
			for (MD_SIZE i = 0; i < size; i++) {
				if (text[i] == '\n') {
					out_append_n(ctx, text + start, i - start);
					out_append(ctx, ANSI_RESET);
					newline_with_prefix(ctx);
					out_append(ctx, ANSI_BG_CODE);
					out_append(ctx, ANSI_GRAY);
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
}

static void debug_log(const char *msg, void *userdata)
{
	(void)userdata;
	(void)msg;
}

/* ---------------- public API ---------------- */
size_t markdown_render_ansi_to_buf(const char *md, char *buf, size_t buf_len)
{
	if (!md)
		return 0;
	if (buf && buf_len > 0)
		buf[0] = '\0';

	struct ansi_ctx ctx = {0};
	sbuf_init(&ctx.out, buf, buf_len);

	MD_PARSER parser = {0};
	parser.abi_version = 0;
	parser.flags = MD_FLAG_STRIKETHROUGH | MD_FLAG_TABLES |
		       MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS |
		       MD_FLAG_COLLAPSEWHITESPACE;
	parser.enter_block = enter_block;
	parser.leave_block = leave_block;
	parser.enter_span = enter_span;
	parser.leave_span = leave_span;
	parser.text = text_callback;
	parser.debug_log = debug_log;

	md_parse(md, (MD_SIZE)strlen(md), &parser, &ctx);

	free(ctx.link_href.buf);
	if (ctx.table)
		free_table(ctx.table);

	if (buf && ctx.out.len < ctx.out.cap)
		buf[ctx.out.len] = '\0';
	return ctx.out.len;
}

static void render_ansi_impl(const char *md, struct ansi_ctx *ctx)
{
	if (!md)
		return;

	MD_PARSER parser = {0};
	parser.abi_version = 0;
	parser.flags = MD_FLAG_STRIKETHROUGH | MD_FLAG_TABLES |
		       MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS |
		       MD_FLAG_COLLAPSEWHITESPACE;
	parser.enter_block = enter_block;
	parser.leave_block = leave_block;
	parser.enter_span = enter_span;
	parser.leave_span = leave_span;
	parser.text = text_callback;
	parser.debug_log = debug_log;

	md_parse(md, (MD_SIZE)strlen(md), &parser, ctx);

	free(ctx->link_href.buf);
	if (ctx->table)
		free_table(ctx->table);
}

static void free_media(struct ansi_ctx *ctx)
{
	for (int i = 0; i < ctx->media_count; i++) {
		free(ctx->media[i].type);
		free(ctx->media[i].path);
	}
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
