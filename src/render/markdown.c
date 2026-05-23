#include "markdown.h"
#include "md4c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>
#include <locale.h>

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

/* Strip ANSI escape sequences, producing a plain-text copy.
 * Returns the byte length of the plain text. */
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

/* Decode a single UTF-8 code point starting at s[pos].
 * Sets *out_byte_len to the byte length of the character.
 * Returns the decoded Unicode code point (0 on error/EOF). */
static unsigned utf8_decode_cp(const char *s, size_t len, size_t pos,
			       size_t *out_byte_len)
{
	if (pos >= len) {
		*out_byte_len = 0;
		return 0;
	}
	unsigned char c = (unsigned char)s[pos];
	unsigned cp = 0;
	size_t bl = 1;
	if (c < 0x80) {
		cp = c;
		bl = 1;
	} else if ((c & 0xE0) == 0xC0 && pos + 1 < len) {
		cp = ((unsigned)(c & 0x1F) << 6) |
		     ((unsigned char)s[pos + 1] & 0x3F);
		bl = 2;
	} else if ((c & 0xF0) == 0xE0 && pos + 2 < len) {
		cp = ((unsigned)(c & 0x0F) << 12) |
		     ((unsigned)((unsigned char)s[pos + 1] & 0x3F) << 6) |
		     ((unsigned char)s[pos + 2] & 0x3F);
		bl = 3;
	} else if ((c & 0xF8) == 0xF0 && pos + 3 < len) {
		cp = ((unsigned)(c & 0x07) << 18) |
		     ((unsigned)((unsigned char)s[pos + 1] & 0x3F) << 12) |
		     ((unsigned)((unsigned char)s[pos + 2] & 0x3F) << 6) |
		     ((unsigned char)s[pos + 3] & 0x3F);
		bl = 4;
	}
	if (pos + bl > len)
		bl = len - pos;
	*out_byte_len = bl;
	return cp;
}

/* Fallback width table for when wcwidth() returns -1 (e.g. C locale).
 * Covers zero-width, CJK wide, and emoji ranges. */
static size_t cp_width_fallback(unsigned cp)
{
	if (cp >= 0x0300 && cp <= 0x036F) return 0;
	if (cp >= 0x0483 && cp <= 0x0489) return 0;
	if (cp >= 0x1AB0 && cp <= 0x1AFF) return 0;
	if (cp >= 0x1DC0 && cp <= 0x1DFF) return 0;
	if (cp >= 0x20D0 && cp <= 0x20FF) return 0;
	if (cp >= 0xFE20 && cp <= 0xFE2F) return 0;
	if (cp == 0x00AD) return 0;
	if (cp == 0x034F) return 0;
	if (cp == 0x200B) return 0;
	if (cp == 0x200C) return 0;
	if (cp == 0x200D) return 0;
	if (cp == 0x2060) return 0;
	if (cp >= 0xFE00 && cp <= 0xFE0F) return 0;
	if (cp >= 0xE0100 && cp <= 0xE01EF) return 0;

	if (cp >= 0x1100 && cp <= 0x11FF) return 2;
	if (cp >= 0x2E80 && cp <= 0x2EFF) return 2;
	if (cp >= 0x2F00 && cp <= 0x2FDF) return 2;
	if (cp >= 0x3040 && cp <= 0x309F) return 2;
	if (cp >= 0x30A0 && cp <= 0x30FF) return 2;
	if (cp >= 0x3100 && cp <= 0x312F) return 2;
	if (cp >= 0x3130 && cp <= 0x318F) return 2;
	if (cp >= 0x3190 && cp <= 0x319F) return 2;
	if (cp >= 0x3200 && cp <= 0x32FF) return 2;
	if (cp >= 0x3300 && cp <= 0x33FF) return 2;
	if (cp >= 0x3400 && cp <= 0x4DBF) return 2;
	if (cp >= 0x4E00 && cp <= 0x9FFF) return 2;
	if (cp >= 0xA000 && cp <= 0xA4CF) return 2;
	if (cp >= 0xAC00 && cp <= 0xD7AF) return 2;
	if (cp >= 0xF900 && cp <= 0xFAFF) return 2;
	if (cp >= 0xFE30 && cp <= 0xFE6F) return 2;
	if (cp >= 0xFF01 && cp <= 0xFFEF) return 2;
	if (cp >= 0x20000 && cp <= 0x2EBEF) return 2;
	if (cp >= 0x2F800 && cp <= 0x2FA1F) return 2;
	if (cp >= 0x30000 && cp <= 0x3134F) return 2;

	if (cp >= 0x1F600 && cp <= 0x1F64F) return 2;
	if (cp >= 0x1F300 && cp <= 0x1F5FF) return 2;
	if (cp >= 0x1F680 && cp <= 0x1F6FF) return 2;
	if (cp >= 0x1F900 && cp <= 0x1F9FF) return 2;
	if (cp >= 0x1FA00 && cp <= 0x1FA6F) return 2;
	if (cp >= 0x1FA70 && cp <= 0x1FAFF) return 2;
	if (cp >= 0x2600 && cp <= 0x26FF) return 2;
	if (cp >= 0x2700 && cp <= 0x27BF) return 2;
	if (cp >= 0x1F000 && cp <= 0x1F02F) return 2;
	if (cp >= 0x1F0A0 && cp <= 0x1F0FF) return 2;

	return 1;
}

/* Return the visible column width of a Unicode code point.
 * Uses wcwidth() which respects LC_CTYPE (e.g. CJK locale
 * treats East Asian Ambiguous chars as width 2).
 * Falls back to hardcoded ranges if wcwidth() returns -1. */
static size_t utf8_cp_width(unsigned cp)
{
	if (cp == 0)
		return 0;
	int w = wcwidth((wchar_t)cp);
	if (w > 0)
		return (size_t)w;
	if (w == 0)
		return 0;
	return cp_width_fallback(cp);
}

/* Count the total visible column width of a UTF-8 string. */
static size_t utf8_visible_cols(const char *s, size_t len)
{
	size_t cols = 0;
	for (size_t i = 0; i < len; ) {
		size_t bl;
		unsigned cp = utf8_decode_cp(s, len, i, &bl);
		cols += utf8_cp_width(cp);
		i += bl ? bl : 1;
	}
	return cols;
}

/* Advance past one UTF-8 character starting at s[pos]. Returns the byte
 * length of the character. Sets *out_cols to the visible column width. */
static size_t utf8_char_at(const char *s, size_t len, size_t pos, size_t *out_cols)
{
	size_t bl;
	unsigned cp = utf8_decode_cp(s, len, pos, &bl);
	*out_cols = utf8_cp_width(cp);
	return bl ? bl : 1;
}

/* Check if the UTF-8 character at s[pos] is a CJK ideograph or wide symbol.
 * Used to identify word boundaries for wrapping: CJK chars are boundaries. */
static int utf8_is_cjk(const char *s, size_t len, size_t pos)
{
	size_t bl;
	unsigned cp = utf8_decode_cp(s, len, pos, &bl);
	/* CJK Unified Ideographs */
	if (cp >= 0x4E00 && cp <= 0x9FFF) return 1;
	if (cp >= 0x3400 && cp <= 0x4DBF) return 1;
	/* CJK Compatibility Ideographs */
	if (cp >= 0xF900 && cp <= 0xFAFF) return 1;
	/* CJK Unified Ideographs Extension B-I */
	if (cp >= 0x20000 && cp <= 0x2EBEF) return 1;
	if (cp >= 0x30000 && cp <= 0x3134F) return 1;
	/* CJK Radicals / Kangxi */
	if (cp >= 0x2E80 && cp <= 0x2EFF) return 1;
	if (cp >= 0x2F00 && cp <= 0x2FDF) return 1;
	/* Bopomofo */
	if (cp >= 0x3100 && cp <= 0x312F) return 1;
	/* Hiragana / Katakana */
	if (cp >= 0x3040 && cp <= 0x309F) return 1;
	if (cp >= 0x30A0 && cp <= 0x30FF) return 1;
	/* Hangul */
	if (cp >= 0xAC00 && cp <= 0xD7AF) return 1;
	if (cp >= 0x1100 && cp <= 0x11FF) return 1;
	if (cp >= 0x3130 && cp <= 0x318F) return 1;
	/* Fullwidth forms */
	if (cp >= 0xFF01 && cp <= 0xFFEF) return 1;
	return 0;
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
	*out_count = 0;
	if (max_cols == 0)
		max_cols = 1;

	unsigned cap = (unsigned)(plain_len + 1);
	if (cap < 4)
		cap = 4;
	struct wrapped_line *lines = calloc(cap, sizeof(struct wrapped_line));
	if (!lines)
		return NULL;

	size_t ri = 0;
	size_t pi = 0;
	size_t line_start = 0;
	size_t line_vis = 0;
	size_t line_count = 0;

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

		size_t char_cols = 0;
		size_t char_bytes = utf8_char_at(plain, plain_len, pi, &char_cols);
		int is_cjk = utf8_is_cjk(plain, plain_len, pi);
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

			if (line_count >= cap) {
				cap *= 2;
				struct wrapped_line *nl = realloc(lines, cap * sizeof(struct wrapped_line));
				if (!nl) {
					for (unsigned k = 0; k < line_count; k++)
						free(lines[k].raw);
					free(lines);
					return NULL;
				}
				lines = nl;
			}
			size_t seg_len = seg_ri - line_start;
			lines[line_count].raw = malloc(seg_len + 1);
			if (!lines[line_count].raw) {
				for (unsigned k = 0; k < line_count; k++)
					free(lines[k].raw);
				free(lines);
				return NULL;
			}
			memcpy(lines[line_count].raw, raw + line_start, seg_len);
			lines[line_count].raw[seg_len] = '\0';
			lines[line_count].raw_len = seg_len;
			lines[line_count].vis_width = seg_vis;
			line_count++;

			line_start = seg_ri;
			line_vis = 0;
			break_ri = line_start;
			break_pi = pi;
			break_vis = 0;

			/* Skip leading spaces on new line */
			while (ri < raw_len && pi < plain_len &&
			       (unsigned char)plain[pi] == ' ') {
				size_t skip_bytes = utf8_char_at(plain, plain_len, pi, &char_cols);
				ri += skip_bytes;
				pi += skip_bytes;
				line_start = ri;
			}
			/* Recalculate current character after skip */
			if (ri >= raw_len)
				break;
			char_cols = 0;
			char_bytes = utf8_char_at(plain, plain_len, pi, &char_cols);
			is_cjk = utf8_is_cjk(plain, plain_len, pi);
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
	if (ri > line_start || line_count == 0) {
		if (line_count >= cap) {
			cap *= 2;
			struct wrapped_line *nl = realloc(lines, cap * sizeof(struct wrapped_line));
			if (!nl) {
				for (unsigned k = 0; k < line_count; k++)
					free(lines[k].raw);
				free(lines);
				return NULL;
			}
			lines = nl;
		}
		size_t seg_len = ri - line_start;
		lines[line_count].raw = malloc(seg_len + 1);
		if (!lines[line_count].raw) {
			for (unsigned k = 0; k < line_count; k++)
				free(lines[k].raw);
			free(lines);
			return NULL;
		}
		memcpy(lines[line_count].raw, raw + line_start, seg_len);
		lines[line_count].raw[seg_len] = '\0';
		lines[line_count].raw_len = seg_len;
		lines[line_count].vis_width = line_vis;
		line_count++;
	}

	*out_count = (unsigned)line_count;
	return lines;
}

static void free_wrapped_lines(struct wrapped_line *lines, unsigned count)
{
	if (!lines)
		return;
	for (unsigned i = 0; i < count; i++)
		free(lines[i].raw);
	free(lines);
}

/* Calculate column widths constrained to terminal width.
 * col_w is initialized with natural (content-max) widths.
 * This function adjusts them to fit within avail_width.
 * First column is preserved as much as possible. */
static void distribute_col_widths(size_t *col_w, unsigned col_count,
				  size_t prefix_width, int term_width)
{
	if (!col_w || col_count == 0)
		return;

	/* Border overhead: each column has "│ " (2 cols) + trailing "│" (1 col)
	 * + 1 space padding after content. Total border = col_count*2 + 1 + col_count
	 * = col_count*3 + 1 */
	size_t border_overhead = (size_t)col_count * 2 + 1 + col_count;
	size_t total_natural = 0;
	for (unsigned c = 0; c < col_count; c++)
		total_natural += col_w[c];

	size_t total_width = prefix_width + border_overhead + total_natural;

	/* If it fits, no adjustment needed */
	if (term_width <= 0 || total_width <= (size_t)term_width)
		return;

	size_t avail = (size_t)term_width;
	if (avail < prefix_width + border_overhead) {
		/* Even the minimal frame doesn't fit — just use natural widths */
		return;
	}
	size_t avail_content = avail - prefix_width - border_overhead;

	/* Minimum column width: 4 cols (at least 2 visible chars + some space) */
	size_t min_w = 4;
	size_t total_min = (size_t)col_count * min_w;
	if (avail_content < total_min) {
		/* Even minimums don't fit — distribute equally */
		size_t each = avail_content / col_count;
		for (unsigned c = 0; c < col_count; c++)
			col_w[c] = each;
		return;
	}

	/* First pass: preserve first column width if possible */
	/* Remaining columns share the rest */
	if (col_w[0] > avail_content - (col_count - 1) * min_w) {
		/* First column must be shrunk too */
		size_t first_max = avail_content - (col_count - 1) * min_w;
		if (first_max < min_w)
			first_max = min_w;
		col_w[0] = first_max < col_w[0] ? first_max : col_w[0];
	}

	/* Shrink columns from right to left until we fit */
	for (int pass = 0; pass < 3; pass++) {
		size_t used = 0;
		for (unsigned c = 0; c < col_count; c++)
			used += col_w[c];
		if (used <= avail_content)
			break;

		size_t excess = used - avail_content;
		for (int c = (int)col_count - 1; c >= (pass == 0 ? 1 : 0); c--) {
			if (col_w[c] <= min_w)
				continue;
			size_t room = col_w[c] - min_w;
			size_t take = room < excess ? room : excess;
			col_w[c] -= take;
			excess -= take;
			if (excess == 0)
				break;
		}
	}
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

	/* Constrain column widths to terminal width */
	size_t prefix_width = (size_t)(ctx->quote_depth * 2);
	distribute_col_widths(col_w, t->col_count, prefix_width,
			      ctx->term_width);

	/* Wrap each cell content to its constrained column width */
	unsigned **wrap_counts = calloc(t->row_count, sizeof(unsigned *));
	struct wrapped_line ***wrap_lines = calloc(t->row_count, sizeof(struct wrapped_line **));
	if (!wrap_counts || !wrap_lines) {
		free(col_w);
		free(wrap_counts);
		free(wrap_lines);
		return;
	}

	for (unsigned r = 0; r < t->row_count; r++) {
		struct table_row *row = &t->rows[r];
		wrap_counts[r] = calloc(t->col_count, sizeof(unsigned));
		wrap_lines[r] = calloc(t->col_count, sizeof(struct wrapped_line *));
		for (unsigned c = 0; c < row->cell_count && c < t->col_count; c++) {
			wrap_lines[r][c] = wrap_cell_content(
				row->cells[c].raw, row->cells[c].raw_len,
				row->cells[c].plain, row->cells[c].plain_len,
				col_w[c], &wrap_counts[r][c]);
		}
	}

	for (unsigned r = 0; r < t->row_count; r++) {
		struct table_row *row = &t->rows[r];
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
	for (unsigned r = 0; r < t->row_count; r++) {
		if (wrap_lines[r]) {
			for (unsigned c = 0; c < t->col_count && c < t->rows[r].cell_count; c++) {
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
	ctx.term_width = get_term_width();

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
