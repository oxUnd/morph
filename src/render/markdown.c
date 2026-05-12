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
#define ANSI_RED       "\033[31m"
#define ANSI_GREEN     "\033[32m"
#define ANSI_YELLOW    "\033[33m"
#define ANSI_BLUE      "\033[34m"
#define ANSI_CYAN      "\033[36m"
#define ANSI_GRAY      "\033[90m"
#define ANSI_BG_CODE   "\033[48;5;236m"

struct ansi_ctx {
	char *buf;
	size_t len;
	size_t cap;
	int bold_depth;
	int italic_depth;
	int code_depth;
	int block_depth;
	int list_depth;
	int in_heading;
	int in_paragraph;
	int new_line_needed;
};

static void ctx_init(struct ansi_ctx *ctx, char *buf, size_t cap)
{
	ctx->buf = buf;
	ctx->cap = cap;
	ctx->len = 0;
	ctx->bold_depth = 0;
	ctx->italic_depth = 0;
	ctx->code_depth = 0;
	ctx->block_depth = 0;
	ctx->list_depth = 0;
	ctx->in_heading = 0;
	ctx->in_paragraph = 0;
	ctx->new_line_needed = 0;
}

static void ctx_append(struct ansi_ctx *ctx, const char *s, size_t n)
{
	if (!ctx->buf)
		return;
	size_t space = ctx->cap > 0 ? ctx->cap - 1 - ctx->len : 0;
	size_t to_write = n < space ? n : space;
	if (to_write > 0) {
		memcpy(ctx->buf + ctx->len, s, to_write);
		ctx->len += to_write;
	}
}

static void ctx_str(struct ansi_ctx *ctx, const char *s)
{
	ctx_append(ctx, s, strlen(s));
}

static int enter_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
	struct ansi_ctx *ctx = userdata;
	switch (type) {
	case MD_BLOCK_H: {
		ctx->in_heading = 1;
		struct MD_BLOCK_H_DETAIL *h = detail;
		ctx_str(ctx, "\n");
		ctx_str(ctx, ANSI_BOLD);
		switch (h->level) {
		case 1: ctx_str(ctx, ANSI_CYAN); break;
		case 2: ctx_str(ctx, ANSI_GREEN); break;
		case 3: ctx_str(ctx, ANSI_YELLOW); break;
		default: break;
		}
		break;
	}
	case MD_BLOCK_CODE:
		ctx->code_depth++;
		ctx_str(ctx, "\n");
		ctx_str(ctx, ANSI_BG_CODE);
		ctx_str(ctx, ANSI_GRAY);
		ctx_str(ctx, "  ");
		break;
	case MD_BLOCK_P:
		ctx->in_paragraph = 1;
		if (ctx->new_line_needed) {
			ctx_str(ctx, "\n");
			ctx->new_line_needed = 0;
		}
		break;
	case MD_BLOCK_QUOTE:
		ctx->block_depth++;
		ctx_str(ctx, ANSI_DIM);
		ctx_str(ctx, "│ ");
		break;
	case MD_BLOCK_UL:
	case MD_BLOCK_OL:
		ctx->list_depth++;
		break;
	case MD_BLOCK_LI:
		ctx_str(ctx, "  ");
		for (int i = 1; i < ctx->list_depth; i++)
			ctx_str(ctx, "  ");
		ctx_str(ctx, "• ");
		break;
	case MD_BLOCK_HR:
		ctx_str(ctx, "\n");
		ctx_str(ctx, ANSI_DIM);
		ctx_str(ctx, "────────────────────");
		ctx_str(ctx, ANSI_RESET);
		ctx_str(ctx, "\n");
		break;
	default:
		break;
	}
	return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
	struct ansi_ctx *ctx = userdata;
	switch (type) {
	case MD_BLOCK_H:
		ctx->in_heading = 0;
		ctx_str(ctx, ANSI_RESET);
		ctx_str(ctx, "\n");
		break;
	case MD_BLOCK_CODE:
		ctx->code_depth--;
		ctx_str(ctx, ANSI_RESET);
		ctx_str(ctx, "\n");
		break;
	case MD_BLOCK_P:
		ctx->in_paragraph = 0;
		ctx->new_line_needed = 1;
		break;
	case MD_BLOCK_QUOTE:
		ctx->block_depth--;
		ctx_str(ctx, ANSI_RESET);
		ctx->new_line_needed = 1;
		break;
	case MD_BLOCK_UL:
	case MD_BLOCK_OL:
		ctx->list_depth--;
		ctx->new_line_needed = 1;
		break;
	case MD_BLOCK_LI:
		ctx->new_line_needed = 1;
		break;
	default:
		break;
	}
	return 0;
}

static int enter_span(MD_SPANTYPE type, void *detail, void *userdata)
{
	struct ansi_ctx *ctx = userdata;
	switch (type) {
	case MD_SPAN_STRONG:
		ctx->bold_depth++;
		ctx_str(ctx, ANSI_BOLD);
		break;
	case MD_SPAN_EM:
		ctx->italic_depth++;
		ctx_str(ctx, ANSI_ITALIC);
		break;
	case MD_SPAN_CODE:
		ctx->code_depth++;
		ctx_str(ctx, ANSI_BG_CODE);
		ctx_str(ctx, ANSI_CYAN);
		break;
	case MD_SPAN_A: {
		struct MD_SPAN_A_DETAIL *a = detail;
		ctx_str(ctx, ANSI_UNDERLINE);
		ctx_str(ctx, ANSI_BLUE);
		break;
	}
	case MD_SPAN_DEL:
		ctx_str(ctx, ANSI_DIM);
		ctx_str(ctx, ANSI_RED);
		break;
	default:
		break;
	}
	return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *userdata)
{
	struct ansi_ctx *ctx = userdata;
	switch (type) {
	case MD_SPAN_STRONG:
		ctx->bold_depth--;
		ctx_str(ctx, ANSI_RESET);
		if (ctx->italic_depth > 0)
			ctx_str(ctx, ANSI_ITALIC);
		break;
	case MD_SPAN_EM:
		ctx->italic_depth--;
		ctx_str(ctx, ANSI_RESET);
		if (ctx->bold_depth > 0)
			ctx_str(ctx, ANSI_BOLD);
		break;
	case MD_SPAN_CODE:
		ctx->code_depth--;
		ctx_str(ctx, ANSI_RESET);
		if (ctx->bold_depth > 0)
			ctx_str(ctx, ANSI_BOLD);
		if (ctx->italic_depth > 0)
			ctx_str(ctx, ANSI_ITALIC);
		if (ctx->block_depth > 0) {
			ctx_str(ctx, ANSI_DIM);
			ctx_str(ctx, "│ ");
		}
		break;
	case MD_SPAN_A:
		ctx_str(ctx, ANSI_RESET);
		break;
	case MD_SPAN_DEL:
		ctx_str(ctx, ANSI_RESET);
		break;
	default:
		break;
	}
	return 0;
}

static int text_callback(MD_TEXTTYPE type, const MD_CHAR *text,
			 MD_SIZE size, void *userdata)
{
	struct ansi_ctx *ctx = userdata;
	switch (type) {
	case MD_TEXT_BR:
		ctx_str(ctx, "\n");
		break;
	case MD_TEXT_SOFTBR:
		ctx_str(ctx, " ");
		break;
	case MD_TEXT_NORMAL:
	case MD_TEXT_CODE:
	case MD_TEXT_ENTITY:
		ctx_append(ctx, text, size);
		break;
	default:
		ctx_append(ctx, text, size);
		break;
	}
	return 0;
}

static void debug_log(const char *msg, void *userdata)
{
	(void)userdata;
	fprintf(stderr, "md4c: %s\n", msg);
}

size_t markdown_render_ansi_to_buf(const char *md, char *buf, size_t buf_len)
{
	if (!md)
		return 0;
	if (buf && buf_len > 0)
		buf[0] = '\0';
	struct ansi_ctx ctx;
	ctx_init(&ctx, buf, buf_len);
	MD_PARSER parser = {0};
	parser.abi_version = 0;
	parser.flags = MD_FLAG_STRIKETHROUGH | MD_FLAG_TABLES |
		       MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS;
	parser.enter_block = enter_block;
	parser.leave_block = leave_block;
	parser.enter_span = enter_span;
	parser.leave_span = leave_span;
	parser.text = text_callback;
	parser.debug_log = debug_log;
	md_parse(md, (MD_SIZE)strlen(md), &parser, &ctx);
	if (buf && ctx.len < ctx.cap)
		buf[ctx.len] = '\0';
	return ctx.len;
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
	markdown_render_ansi_to_buf(md, buf, buf_len);
	printf("%s\n", buf);
	fflush(stdout);
	free(buf);
}