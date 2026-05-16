#include "cli.h"
#include "util/log.h"
#include "util/file.h"
#include "util/spin.h"
#include "util/arena.h"
#include "agent/tokenizer.h"
#include "agent/compress.h"
#include "agent/tools/text_gen.h"
#include "ext/ext.h"
#include "agent/tools/text_qa.h"
#include "agent/tools/img_gen.h"
#include "agent/tools/img_edit.h"
#include "agent/tools/img_info.h"
#include "agent/tools/img_resize.h"
#include "agent/tools/img_convert.h"
#include "agent/tools/vid_gen.h"
#include "agent/tools/file_read.h"
#include "agent/tools/file_list.h"
#include "agent/tools/file_info.h"
#include "agent/tools/skill_activate.h"
#include "agent/tools/bash_exec.h"
#include "db/database.h"
#include "config.h"
#include "render/markdown.h"
#include "render/image.h"
#include "render/video.h"
#include "stb_image.h"
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <ctype.h>

#ifdef HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

static void media_callback(const char *type, const char *path, void *user);

#define ANSI_BOLD   "\033[1m"
#define ANSI_DIM    "\033[2m"
#define ANSI_RED    "\033[31m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN   "\033[36m"
#define ANSI_RESET  "\033[0m"

#define CMD_HEADER(fmt, ...) \
	printf(ANSI_BOLD ANSI_CYAN "--- " fmt ANSI_RESET "\n", ##__VA_ARGS__)
#define CMD_ERROR(fmt, ...) \
	printf(ANSI_BOLD ANSI_RED "error: " ANSI_RESET fmt "\n", ##__VA_ARGS__)
#define CMD_OK(fmt, ...) \
	printf(ANSI_GREEN fmt ANSI_RESET "\n", ##__VA_ARGS__)

static const char *default_db_path = "~/.morph/data.db";
static const char *default_config_path = "~/.morph/config.toml";

static size_t utf8_truncate(const char *s, size_t max_bytes)
{
	if (!s) return 0;
	size_t len = strlen(s);
	if (len <= max_bytes) return len;
	/* Walk backward from max_bytes to find a valid UTF-8 start byte.
	 * Continuation bytes match 0x80-0xBF; start bytes are 0xC0+ or ASCII <0x80. */
	size_t pos = max_bytes;
	while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80)
		pos--;
	return pos;
}

/* Remove invalid UTF-8 byte sequences in-place.
 * Valid sequences are kept; invalid bytes are simply removed.
 * This ensures the string is always safe for libedit / readline rendering. */
/* Load stored messages from DB into react context.
 * Clears any existing in-memory messages first. */
static void session_load_history(struct cli_context *ctx)
{
	if (!ctx || !ctx->react)
		return;
	msg_list_destroy(ctx->react->messages);
	ctx->react->messages = NULL;
	int count = 0;
	struct message *list = message_list(&ctx->database, ctx->current_session.id, &count);
	struct message *cur = list;
	while (cur) {
		struct message_list *m = msg_list_create(cur->role, cur->content,
							  cur->token_count);
		if (m) {
			m->compressed = cur->compressed;
			msg_list_append(&ctx->react->messages, m);
		}
		cur = cur->next;
	}
	message_free_list(list);
}

static void utf8_sanitize_inplace(char *s)
{
	if (!s) return;
	unsigned char *p = (unsigned char *)s;
	size_t r = 0, w = 0;
	while (p[r]) {
		unsigned char c = p[r];
		if (c < 0x80) {
			p[w++] = p[r++];
		} else if ((c & 0xE0) == 0xC0) {
			if ((p[r+1] & 0xC0) == 0x80) {
				p[w++] = p[r++];
				p[w++] = p[r++];
			} else {
				r++;
			}
		} else if ((c & 0xF0) == 0xE0) {
			if ((p[r+1] & 0xC0) == 0x80 && (p[r+2] & 0xC0) == 0x80) {
				p[w++] = p[r++];
				p[w++] = p[r++];
				p[w++] = p[r++];
			} else {
				r++;
			}
		} else if ((c & 0xF8) == 0xF0) {
			if ((p[r+1] & 0xC0) == 0x80 && (p[r+2] & 0xC0) == 0x80 && (p[r+3] & 0xC0) == 0x80) {
				p[w++] = p[r++];
				p[w++] = p[r++];
				p[w++] = p[r++];
				p[w++] = p[r++];
			} else {
				r++;
			}
		} else {
			r++;
		}
	}
	p[w] = '\0';
}

static int utf8_display_width(const char *s)
{
	if (!s) return 0;
	int w = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		uint32_t cp;
		int bytes;
		if (*p < 0x80) {
			cp = *p; bytes = 1;
		} else if ((*p & 0xE0) == 0xC0) {
			cp = *p & 0x1F; bytes = 2;
		} else if ((*p & 0xF0) == 0xE0) {
			cp = *p & 0x0F; bytes = 3;
		} else if ((*p & 0xF8) == 0xF0) {
			cp = *p & 0x07; bytes = 4;
		} else {
			p++; continue;
		}
		for (int i = 1; i < bytes; i++) {
			if ((p[i] & 0xC0) != 0x80) { bytes = 0; break; }
		}
		if (bytes == 0) { p++; continue; }
		for (int i = 1; i < bytes; i++)
			cp = (cp << 6) | (p[i] & 0x3F);
		p += bytes;
		if (cp < 0x20) continue;
		if ((cp >= 0x1100 && cp <= 0x115F) ||
		    (cp >= 0x2329 && cp <= 0x232A) ||
		    (cp >= 0x2E80 && cp <= 0x303E) ||
		    (cp >= 0x3040 && cp <= 0x334F) ||
		    (cp >= 0x3400 && cp <= 0x4DBF) ||
		    (cp >= 0x4E00 && cp <= 0x9FFF) ||
		    (cp >= 0xA960 && cp <= 0xA97C) ||
		    (cp >= 0xAC00 && cp <= 0xD7A3) ||
		    (cp >= 0xD7B0 && cp <= 0xD7C6) ||
		    (cp >= 0xF900 && cp <= 0xFAFF) ||
		    (cp >= 0xFE10 && cp <= 0xFE19) ||
		    (cp >= 0xFE30 && cp <= 0xFE6F) ||
		    (cp >= 0xFF01 && cp <= 0xFF60) ||
		    (cp >= 0xFFE0 && cp <= 0xFFE6) ||
		    (cp >= 0x20000 && cp <= 0x2FFFD) ||
		    (cp >= 0x30000 && cp <= 0x3FFFD))
			w += 2;
		else
			w += 1;
	}
	return w;
}

static void print_padded(const char *s, int target_width)
{
	if (!s) s = "";
	int dw = utf8_display_width(s);
	fputs(s, stdout);
	int pad = target_width - dw;
	for (int i = 0; i < pad; i++)
		putchar(' ');
}

/* ---- arg/argv helpers ---- */

static int argv_split(const char *input, char **argv, int max_args)
{
	int argc = 0;
	if (!input || max_args < 1)
		return 0;
	while (*input && argc < max_args - 1) {
		while (*input && isspace((unsigned char)*input))
			input++;
		if (!*input)
			break;
		if (*input == '"' || *input == '\'') {
			char quote = *input++;
			argv[argc++] = (char *)input;
			while (*input && *input != quote)
				input++;
			if (*input)
				*(char *)input++ = '\0';
		} else {
			argv[argc++] = (char *)input;
			while (*input && !isspace((unsigned char)*input))
				input++;
			if (*input)
				*(char *)input++ = '\0';
		}
	}
	argv[argc] = NULL;
	return argc;
}

static const char *cmd_arg(int argc, char **argv, int idx)
{
	return (idx >= 0 && idx < argc) ? argv[idx] : NULL;
}

/* ---- command handlers (forward declarations) ---- */
struct cli_context;

static int cmd_quit(struct cli_context *ctx, int argc, char **argv);
static int cmd_help(struct cli_context *ctx, int argc, char **argv);
static int cmd_new(struct cli_context *ctx, int argc, char **argv);
static int cmd_switch(struct cli_context *ctx, int argc, char **argv);
static int cmd_list(struct cli_context *ctx, int argc, char **argv);
static int cmd_rename(struct cli_context *ctx, int argc, char **argv);
static int cmd_delete(struct cli_context *ctx, int argc, char **argv);
static int cmd_history(struct cli_context *ctx, int argc, char **argv);
static int cmd_model(struct cli_context *ctx, int argc, char **argv);
static int cmd_trace(struct cli_context *ctx, int argc, char **argv);
static int cmd_context(struct cli_context *ctx, int argc, char **argv);
static int cmd_compress(struct cli_context *ctx, int argc, char **argv);
static int cmd_save(struct cli_context *ctx, int argc, char **argv);
static int cmd_config(struct cli_context *ctx, int argc, char **argv);
static int cmd_image(struct cli_context *ctx, int argc, char **argv);
static int cmd_video(struct cli_context *ctx, int argc, char **argv);
static int cmd_ext(struct cli_context *ctx, int argc, char **argv);
static int cmd_skill(struct cli_context *ctx, int argc, char **argv);
static int cmd_export_alias(struct cli_context *ctx, int argc, char **argv);

/* ---- dispatch table ---- */

static int cmd_render(struct cli_context *ctx, int argc, char **argv);

struct cmd_entry {
	const char *name;
	int (*handler)(struct cli_context *ctx, int argc, char **argv);
	const char *desc;
	const char *usage;
	/* TODO: add `is_alias` flag to cmd_entry so help rendering doesn't need
	 *       to match desc against "Alias for" prefix — fragile and not i18n-safe */
};

static const struct cmd_entry commands[] = {
	{ "/quit",    cmd_quit,    "Exit the program",                  "" },
	{ "/q",       cmd_quit,    "Alias for /quit",                   "" },
	{ "/help",    cmd_help,    "Show help for commands",            "/help [command]" },
	{ "/h",       cmd_help,    "Alias for /help",                   "/h [command]" },
	{ "/new",     cmd_new,     "Create a new session",              "/new [name]" },
	{ "/n",       cmd_new,     "Alias for /new",                    "/n [name]" },
	{ "/switch",  cmd_switch,  "Switch to another session",         "/switch <name|id>" },
	{ "/s",       cmd_switch,  "Alias for /switch",                 "/s <name|id>" },
	{ "/list",    cmd_list,    "List all sessions",                 "/list" },
	{ "/ls",      cmd_list,    "Alias for /list",                   "/ls" },
	{ "/rename",  cmd_rename,  "Rename current session",            "/rename <new_name>" },
	{ "/rn",      cmd_rename,  "Alias for /rename",                 "/rn <new_name>" },
	{ "/delete",  cmd_delete,  "Delete a session",                  "/delete <name|id>" },
	{ "/del",     cmd_delete,  "Alias for /delete",                 "/del <name|id>" },
	{ "/history", cmd_history, "Show recent messages",              "/history [n|--all]" },
	{ "/hi",      cmd_history, "Alias for /history",                "/hi [n|--all]" },
	{ "/model",   cmd_model,   "View or switch the LLM model",      "/model [name]" },
	{ "/m",       cmd_model,   "Alias for /model",                   "/m [name]" },
	{ "/trace",   cmd_trace,   "Show ReAct trace for current turn", "/trace [--from-db]" },
	{ "/t",       cmd_trace,   "Alias for /trace",                   "/t [--from-db]" },
	{ "/context", cmd_context, "Show token usage and context info", "/context" },
	{ "/ctx",     cmd_context, "Alias for /context",                 "/ctx" },
	{ "/compress",cmd_compress,"Manually compress context window",  "/compress" },
	{ "/cp",      cmd_compress,"Alias for /compress",                "/cp" },
	{ "/save",    cmd_save,    "Export session to a file",          "/save [format]" },
	{ "/config",  cmd_config,  "View current configuration",        "/config" },
	{ "/cfg",     cmd_config,  "Alias for /config",                 "/cfg" },
	{ "/image",   cmd_image,   "Inject an image into context",      "/image <file_path>" },
	{ "/img",     cmd_image,   "Alias for /image",                   "/img <file_path>" },
	{ "/video",   cmd_video,   "Inject a video (M3)",               "/video <file_path>" },
	{ "/vid",     cmd_video,   "Alias for /video",                  "/vid <file_path>" },
	{ "/ext",     cmd_ext,     "List or manage tools and exts",     "/ext list" },
	{ "/x",       cmd_ext,     "Alias for /ext",                    "/x list" },
	{ "/skill",   cmd_skill,   "List or manage skills",             "/skill list" },
	{ "/sk",      cmd_skill,   "Alias for /skill",                  "/sk list" },
	{ "/render",  cmd_render,  "Render a file (image/video/markdown)", "/render <file_path>" },
	{ "/r",       cmd_render,  "Alias for /render",                  "/r <file_path>" },
	{ "/export",  cmd_export_alias, "Alias for /save",              "/export <format>" },
};

static const int num_commands = (int)(sizeof(commands) / sizeof(commands[0]));

/* ---- ext_run wrapper ---- */

static int ext_run_wrapper(const char *args_json, char **result_json, void *user_data)
{
	struct ext *ex = user_data;
	if (!ex)
		return -EINVAL;
	return ext_run(ex, args_json, result_json);
}

static const struct cmd_entry *cmd_lookup(const char *name)
{
	if (!name)
		return NULL;
	for (int i = 0; i < num_commands; i++) {
		if (strcmp(commands[i].name, name) == 0)
			return &commands[i];
	}
	return NULL;
}

/* ---- command implementations ---- */

static int cmd_quit(struct cli_context *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	ctx->running = 0;
	CMD_OK("Goodbye!");
	return 0;
}

static int cmd_help(struct cli_context *ctx, int argc, char **argv)
{
	(void)ctx;
	const char *topic = cmd_arg(argc, argv, 1);
	if (topic) {
		if (topic[0] != '/') {
			char full[64];
			snprintf(full, sizeof(full), "/%s", topic);
			topic = full;
		}
		const struct cmd_entry *e = cmd_lookup(topic);
		if (!e) {
			CMD_ERROR("unknown command: %s", topic);
			return -ENOENT;
		}
		printf(ANSI_BOLD "  %s" ANSI_RESET " — %s\n", e->name, e->desc);
		if (e->usage && *e->usage)
			printf("  " ANSI_DIM "usage: %s" ANSI_RESET "\n", e->usage);
		return 0;
	}
	printf(ANSI_BOLD "morph commands:" ANSI_RESET "\n");
	for (int i = 0; i < num_commands; i++) {
		const char *desc = commands[i].desc;
		int alias_end = i;
		for (int j = i + 1; j < num_commands; j++) {
			if (commands[j].handler != commands[i].handler)
				break;
			alias_end = j;
		}
		/* TODO: consider grouping aliases via is_alias flag instead of relying
	 *       on adjacent entries sharing the same handler — fragile if table
	 *       gets reordered */
	if (alias_end > i && strncmp(desc, "Alias for", 9) == 0)
			desc = commands[alias_end].desc;
		const char *pri = commands[i].name;
		const char *alt = (alias_end > i) ? commands[i + 1].name : NULL;
		char display[128];
		if (alt) {
			const char *shorter = (strlen(pri) <= strlen(alt)) ? pri : alt;
			const char *longer  = (strlen(pri) <= strlen(alt)) ? alt : pri;
			const char *s = shorter + 1;
			const char *l = longer + 1;
			size_t slen = strlen(s);
			if (strncmp(s, l, slen) == 0)
				snprintf(display, sizeof(display), "/%s[%s]", s, l + slen);
			else
				snprintf(display, sizeof(display), "%s,%s", shorter, longer);
		} else {
			snprintf(display, sizeof(display), "%s", pri);
		}
		printf("  ");
		print_padded(display, 24);
		printf("%s\n", desc);
		i = alias_end;
	}
	return 0;
}

static int cmd_new(struct cli_context *ctx, int argc, char **argv)
{
	const char *name = cmd_arg(argc, argv, 1);
	if (!name)
		name = "new_session";
	struct session s;
	int rc = session_create(&ctx->database, name,
				ctx->config.models.text.model, &s);
	if (rc == 0) {
		ctx->current_session = s;
		utf8_sanitize_inplace(ctx->current_session.name);
		session_load_history(ctx);
		ctx->session_auto_named = (strcmp(name, "new_session") == 0);
		CMD_OK("created and switched to session: %s [%s]", name,
		       ctx->current_session.display_id);
	} else {
		CMD_ERROR("failed to create session: %s", name);
	}
	return rc;
}

static int cmd_switch(struct cli_context *ctx, int argc, char **argv)
{
	const char *name = cmd_arg(argc, argv, 1);
	if (!name) {
		CMD_ERROR("usage: /switch <name|id|display_id>");
		return -EINVAL;
	}
	struct session s;
	int rc = session_get_by_name(&ctx->database, name, &s);
	if (rc < 0)
		rc = session_get_by_display_id(&ctx->database, name, &s);
	if (rc < 0) {
		char *end;
		long id = strtol(name, &end, 10);
		if (*end == '\0')
			rc = session_get_by_id(&ctx->database, (int64_t)id, &s);
	}
	if (rc == 0) {
		ctx->current_session = s;
		utf8_sanitize_inplace(ctx->current_session.name);
		session_load_history(ctx);
		ctx->session_auto_named = 1;
		CMD_OK("switched to session: %s", ctx->current_session.name);
	} else {
		CMD_ERROR("session not found: %s", name);
	}
	return rc;
}

static int cmd_list(struct cli_context *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	struct session *list;
	int count = 0;
	session_list(&ctx->database, &list, &count);
	CMD_HEADER("sessions (%d)", count);
	printf("  ");
	print_padded("ID", 10); putchar(' ');
	print_padded("Name", 45); putchar(' ');
	print_padded("Model", 30); putchar(' ');
	printf("Tokens\n");
	printf("  ");
	print_padded("---", 10); putchar(' ');
	print_padded("---", 45); putchar(' ');
	print_padded("---", 30); putchar(' ');
	printf("---\n");
	for (int i = 0; i < count; i++) {
		int is_current = (list[i].id == ctx->current_session.id);
		const char *model = is_current ? ctx->config.models.text.model : list[i].model;
		printf("  ");
		if (is_current) fputs(ANSI_GREEN, stdout);
		print_padded(list[i].display_id, 10);
		if (is_current) fputs(ANSI_RESET, stdout);
		putchar(' ');
		print_padded(list[i].name, 45); putchar(' ');
		print_padded(model, 30); putchar(' ');
		printf("%lld\n", (long long)list[i].token_used);
	}
	free(list);
	return 0;
}

static int cmd_rename(struct cli_context *ctx, int argc, char **argv)
{
	const char *new_name = cmd_arg(argc, argv, 1);
	if (!new_name) {
		CMD_ERROR("usage: /rename <new_name>");
		return -EINVAL;
	}
	int rc = session_rename(&ctx->database, ctx->current_session.id, new_name);
	if (rc == 0) {
		strncpy(ctx->current_session.name, new_name,
			sizeof(ctx->current_session.name) - 1);
		utf8_sanitize_inplace(ctx->current_session.name);
		CMD_OK("session renamed to: %s", new_name);
	} else {
		CMD_ERROR("failed to rename session");
	}
	return rc;
}

static int cmd_delete(struct cli_context *ctx, int argc, char **argv)
{
	const char *name = cmd_arg(argc, argv, 1);
	if (!name) {
		CMD_ERROR("usage: /delete <name|id>");
		return -EINVAL;
	}
	int64_t id = -1;
	struct session s;
	if (session_get_by_name(&ctx->database, name, &s) == 0)
		id = s.id;
	else {
		char *end;
		id = strtol(name, &end, 10);
		if (*end != '\0')
			id = -1;
	}
	if (id < 0) {
		CMD_ERROR("session not found: %s", name);
		return -ENOENT;
	}
	if (id == ctx->current_session.id) {
		CMD_ERROR("cannot delete current session, switch first");
		return -EINVAL;
	}
	int rc = session_delete(&ctx->database, id);
	if (rc == 0)
		CMD_OK("deleted session: %s", name);
	else
		CMD_ERROR("failed to delete session");
	return rc;
}

static int cmd_history(struct cli_context *ctx, int argc, char **argv)
{
	int n = 20;
	int show_all = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--all") == 0 || strcmp(argv[i], "--from-db") == 0)
			show_all = 1;
		else if (argv[i][0] && isdigit((unsigned char)argv[i][0]))
			n = atoi(argv[i]);
	}
	if (n <= 0)
		n = 20;
	int count = 0;
	struct message *msgs = message_list(&ctx->database,
					     ctx->current_session.id, &count);
	int show = show_all ? count : (n > count ? count : n);
	int skip = show_all ? 0 : (count - show);
	CMD_HEADER("%s (showing %s %d of %d)", ctx->current_session.name,
		   show_all ? "all" : "last", show, count);
	struct message *cur = msgs;
	for (int i = 0; i < skip && cur; i++)
		cur = cur->next;
	while (cur) {
		const char *role = cur->role;
		const char *label = (strcmp(role, "user") == 0) ? "You" :
				   (strcmp(role, "assistant") == 0) ? "AI" : role;
		printf(ANSI_DIM "[%s]" ANSI_RESET " ", label);
		if (strcmp(role, "assistant") == 0 && cur->content && *cur->content)
			markdown_render_ansi_with_media(cur->content, media_callback, NULL);
		else
			printf("%s\n", cur->content ? cur->content : "(empty)");
		cur = cur->next;
	}
	message_free_list(msgs);
	return 0;
}

static int cmd_model(struct cli_context *ctx, int argc, char **argv)
{
	const char *name = cmd_arg(argc, argv, 1);
	if (name) {
		strncpy(ctx->current_session.model, name,
			sizeof(ctx->current_session.model) - 1);
		session_update_model(&ctx->database, ctx->current_session.id, name);
		if (ctx->llm)
			strncpy(ctx->llm->model_id, name,
				sizeof(ctx->llm->model_id) - 1);
		CMD_OK("model switched to: %s", name);
	} else {
		printf("current model: %s\n", ctx->current_session.model);
	}
	return 0;
}

static void print_trace_steps(struct react_step *steps, int count, const char *state_name)
{
	struct react_step *cur = steps;
	int step = 1;
	while (cur) {
		const char *color = "";
		switch (cur->type) {
		case REACT_STEP_THOUGHT:	color = ANSI_DIM; break;
		case REACT_STEP_ACTION:		color = ANSI_YELLOW; break;
		case REACT_STEP_OBSERVATION:	color = ANSI_DIM; break;
		case REACT_STEP_REFLECTION:	color = ANSI_CYAN; break;
		case REACT_STEP_FINAL:		color = ANSI_GREEN; break;
		default:			color = ""; break;
		}
		printf("  %d. %s[%s]%s", step, color, react_step_type_name(cur->type), ANSI_RESET);
		if (cur->content)
			printf(" %s", cur->content);
		if (cur->tool_name)
			printf(ANSI_DIM " (tool: %s)" ANSI_RESET, cur->tool_name);
		printf("\n");
		cur = cur->next;
		step++;
	}
	printf(ANSI_DIM "state: %s, steps: %d" ANSI_RESET "\n",
	       state_name ? state_name : "n/a", count);
}

static struct react_step *json_to_react_steps(struct arena *arena, const char *json, int *out_count)
{
	if (!json)
		return NULL;
	cJSON *arr = cJSON_Parse(json);
	if (!cJSON_IsArray(arr))
		return NULL;
	int count = cJSON_GetArraySize(arr);
	struct react_step head = {0};
	struct react_step *tail = &head;
	for (int i = 0; i < count; i++) {
		cJSON *obj = cJSON_GetArrayItem(arr, i);
		if (!cJSON_IsObject(obj))
			continue;
		cJSON *type_item = cJSON_GetObjectItem(obj, "type");
		const char *type_name = cJSON_IsString(type_item) ? type_item->valuestring : "Unknown";
		enum react_step_type type = REACT_STEP_THOUGHT;
		if (strcmp(type_name, "Thought") == 0)		type = REACT_STEP_THOUGHT;
		else if (strcmp(type_name, "Action") == 0)	type = REACT_STEP_ACTION;
		else if (strcmp(type_name, "Observation") == 0)	type = REACT_STEP_OBSERVATION;
		else if (strcmp(type_name, "Reflection") == 0)	type = REACT_STEP_REFLECTION;
		else if (strcmp(type_name, "Final") == 0)	type = REACT_STEP_FINAL;
		cJSON *content = cJSON_GetObjectItem(obj, "content");
		cJSON *tool_name = cJSON_GetObjectItem(obj, "tool_name");
		cJSON *tool_args = cJSON_GetObjectItem(obj, "tool_args");
		cJSON *tool_call_id = cJSON_GetObjectItem(obj, "tool_call_id");
		char args_buf[1024] = {0};
		if (cJSON_IsString(tool_args) && tool_args->valuestring)
			snprintf(args_buf, sizeof(args_buf), "%s(%s)",
				 cJSON_IsString(tool_name) ? tool_name->valuestring : "",
				 tool_args->valuestring);
		struct react_step *s = react_step_create(
			arena,
			type,
			cJSON_IsString(content) ? content->valuestring : NULL,
			cJSON_IsString(tool_name) ? tool_name->valuestring : NULL,
			args_buf[0] ? args_buf : NULL,
			cJSON_IsString(tool_call_id) ? tool_call_id->valuestring : NULL);
		if (s) {
			tail->next = s;
			tail = s;
		}
	}
	cJSON_Delete(arr);
	if (out_count)
		*out_count = count;
	return head.next;
}

static int cmd_trace(struct cli_context *ctx, int argc, char **argv)
{
	int from_db = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--from-db") == 0 || strcmp(argv[i], "--all") == 0)
			from_db = 1;
	}
	if (from_db) {
		int round_no = 0, aborted = 0;
		char *json = trace_load_latest(&ctx->database, ctx->current_session.id,
					      &round_no, &aborted);
		if (!json) {
			printf("no traces saved in DB for this session\n");
			return 0;
		}
		CMD_HEADER("ReAct trace (round %d, %s)", round_no,
			   aborted ? "aborted" : "completed");
		int count = 0;
		struct arena *arena = arena_create(64 * 1024);
		struct react_step *steps = json_to_react_steps(arena, json, &count);
		if (steps) {
			print_trace_steps(steps, count, aborted ? "ABORT" : "DONE");
		} else {
			printf("  (raw) %s\n", json);
		}
		arena_destroy(arena);
		free(json);
		return 0;
	}
	if (!ctx->react || !ctx->react->steps) {
		printf("no ReAct trace for current turn\n");
		return 0;
	}
	CMD_HEADER("ReAct trace (%s)", ctx->current_session.name);
	print_trace_steps(ctx->react->steps, ctx->react->step_count,
			  react_state_name(ctx->react->state));
	return 0;
}

static int cmd_context(struct cli_context *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	int msg_count = message_count(&ctx->database, ctx->current_session.id);
	int total_tokens = 0;
	int limit = ctx->tokenizer ? ctx->tokenizer->context_limit : 0;
	struct message *msgs = message_list(&ctx->database,
					    ctx->current_session.id, &msg_count);
	struct message *cur = msgs;
	while (cur) {
		total_tokens += cur->token_count;
		cur = cur->next;
	}
	message_free_list(msgs);
	double pct = limit > 0 ? (double)total_tokens / limit * 100.0 : 0.0;
	printf("context: %s%d / %d tokens (%.1f%%)%s | messages: %d\n",
	       pct >= 80.0 ? ANSI_YELLOW : "",
	       total_tokens, limit, pct,
	       pct >= 80.0 ? ANSI_RESET : "",
	       msg_count);
	return 0;
}

static int cmd_compress(struct cli_context *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	int count = 0;
	struct message *msgs = message_list(&ctx->database,
					    ctx->current_session.id, &count);
	if (!msgs || count == 0) {
		printf("no messages to compress\n");
		message_free_list(msgs);
		return 0;
	}
	int keep = ctx->config.context.keep_recent_rounds * 2;
	if (count <= keep) {
		CMD_OK("only %d messages, no compression needed (keep %d)", count, keep);
		message_free_list(msgs);
		return 0;
	}

	/* Build an in-memory list mirroring DB rows so we can apply the
	 * layered compression fallback (REQUIREMENTS §6.3): react_trace →
	 * sliding_window. Track which DB ids survive; everything else is
	 * deleted from the DB. */
	struct message_list *head = NULL;
	int *ids = calloc((size_t)count, sizeof(*ids));
	int n_ids = 0;
	for (struct message *m = msgs; m; m = m->next) {
		struct message_list *node = msg_list_create(m->role, m->content,
							    m->token_count);
		if (!node)
			continue;
		node->compressed = m->compressed;
		msg_list_append(&head, node);
		ids[n_ids++] = (int)m->id;
	}
	message_free_list(msgs);

	struct compress_result trace_res = {0};
	(void)compress_react_trace(&head, &trace_res);
	struct compress_result win_res = {0};
	int rc = compress_sliding_window(&head,
		ctx->config.context.keep_recent_rounds, &win_res);
	if (rc < 0) {
		msg_list_destroy(head);
		free(ids);
		CMD_ERROR("compression failed: %d", rc);
		return rc;
	}

	/* Compute survivors by walking remaining list and matching content
	 * with the original DB rows (preserving order).  Entries from the
	 * tail of `ids` correspond to the last messages, which sliding_window
	 * keeps; we delete the prefix that was dropped. */
	int kept = msg_list_count(head);
	int removed = count - kept;
	for (int i = 0; i < removed; i++) {
		/* Best-effort: delete by id; keep going on failure. */
		(void)message_delete(&ctx->database, ids[i]);
	}
	msg_list_destroy(head);
	free(ids);

	/* Refresh in-memory react context from DB. */
	session_load_history(ctx);

	CMD_OK("compressed: react_trace removed %d, sliding_window removed %d, kept %d",
	       trace_res.messages_removed, win_res.messages_removed, kept);
	return 0;
}

static int cmd_save(struct cli_context *ctx, int argc, char **argv)
{
	const char *fmt = cmd_arg(argc, argv, 1);
	if (!fmt)
		fmt = "md";
	int count = 0;
	struct message *msgs = message_list(&ctx->database,
					     ctx->current_session.id, &count);
	char filename[512];
	snprintf(filename, sizeof(filename), "%s_%lld.%s",
		 ctx->current_session.name,
		 (long long)time(NULL), fmt);
	CMD_HEADER("saving session to %s", filename);
	FILE *f = fopen(filename, "w");
	if (f) {
		fprintf(f, "# Session: %s\n\n", ctx->current_session.name);
		struct message *cur = msgs;
		while (cur) {
			fprintf(f, "**%s**: %s\n\n", cur->role,
				cur->content ? cur->content : "");
			cur = cur->next;
		}
		fclose(f);
		CMD_OK("saved %d messages", count);
	} else {
		CMD_ERROR("failed to open file for writing");
	}
	message_free_list(msgs);
	return 0;
}

static int cmd_config(struct cli_context *ctx, int argc, char **argv)
{
	(void)argc;
	(void)argv;
	printf(ANSI_BOLD "[general]" ANSI_RESET "\n");
	printf("  default_session = %s\n", ctx->config.general.default_session);
	printf("  output_dir = %s\n", ctx->config.general.output_dir);
	printf("  log_level = %s\n", ctx->config.general.log_level);
	printf(ANSI_BOLD "[model.text]" ANSI_RESET "\n");
	printf("  provider = %s\n", ctx->config.models.text.provider);
	printf("  model = %s\n", ctx->config.models.text.model);
	printf("  api_base = %s\n", ctx->config.models.text.api_base);
	printf("  context_limit = %d\n", ctx->config.models.text.context_limit);
	printf(ANSI_BOLD "[react]" ANSI_RESET "\n");
	printf("  max_iterations = %d\n", ctx->config.react.max_iterations);
	printf("  step_timeout = %d\n", ctx->config.react.step_timeout_seconds);
	printf("  tool_max_retries = %d\n", ctx->config.react.tool_max_retries);
	printf("  guardrail_enabled = %d\n", ctx->config.react.guardrail_enabled);
	printf("  guardrail_max_retries = %d\n", ctx->config.react.guardrail_max_retries);
	printf(ANSI_BOLD "[context]" ANSI_RESET "\n");
	printf("  threshold = %.1f\n", ctx->config.context.summarize_threshold_ratio);
	printf("  target = %.1f\n", ctx->config.context.compress_target_ratio);
	printf("  keep_rounds = %d\n", ctx->config.context.keep_recent_rounds);
	return 0;
}

static int cmd_image(struct cli_context *ctx, int argc, char **argv)
{
	const char *path = cmd_arg(argc, argv, 1);
	if (!path) {
		CMD_ERROR("usage: /image <file_path>");
		return -EINVAL;
	}
	char *expanded = file_expand_path(path);
	if (!file_exists(expanded)) {
		CMD_ERROR("file not found: %s", expanded);
		free(expanded);
		return -ENOENT;
	}
	int w = 0, h = 0, ch = 0;
	if (!stbi_info(expanded, &w, &h, &ch)) {
		CMD_ERROR("not a valid image file: %s", expanded);
		free(expanded);
		return -EIO;
	}
	strncpy(ctx->image_path, expanded, sizeof(ctx->image_path) - 1);
	image_render_terminal(expanded);
	CMD_OK("image loaded: %s (%dx%d, %d channels)", expanded, w, h, ch);
	free(expanded);
	return 0;
}

static int cmd_video(struct cli_context *ctx, int argc, char **argv)
{
	if (argc < 2) {
		CMD_ERROR("usage: /video <file_path>");
		return -EINVAL;
	}
	if (!file_exists(argv[1])) {
		CMD_ERROR("file not found: %s", argv[1]);
		return -ENOENT;
	}
	if (video_play(argv[1], ctx->config.render.mpv_args) != 0) {
		CMD_ERROR("failed to play video: %s", argv[1]);
		return -EIO;
	}
	CMD_OK("video loaded: %s", argv[1]);
	return 0;
}

static int cmd_ext(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cmd_arg(argc, argv, 1);
	if (sub && strcmp(sub, "list") == 0) {
		CMD_HEADER("registered tools (%d)", ctx->tools.count);
		for (int i = 0; i < ctx->tools.count; i++) {
			printf("  %-15s %s\n",
			       ctx->tools.entries[i].desc.name,
			       ctx->tools.entries[i].desc.desc);
		}
		if (ctx->tools.count == 0)
			printf("  (none)\n");
		return 0;
	}
	if (sub && strcmp(sub, "info") == 0) {
		const char *name = cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /ext info <name>");
			return -EINVAL;
		}
		struct tool_entry *e = tool_lookup(&ctx->tools, name);
		if (!e) {
			CMD_ERROR("tool not found: %s", name);
			return -ENOENT;
		}
		printf("  %-15s %s\n", "Name", e->desc.name);
		printf("  %-15s %s\n", "Description", e->desc.desc);
		if (e->desc.args_spec[0])
			printf("  %-15s %s\n", "Args spec", e->desc.args_spec);
		return 0;
	}
	if (sub && strcmp(sub, "install") == 0) {
		CMD_ERROR("ext install not yet implemented (M4)");
		return 0;
	}
	if (sub && strcmp(sub, "remove") == 0) {
		CMD_ERROR("ext remove not yet implemented (M4)");
		return 0;
	}
	if (sub && strcmp(sub, "enable") == 0) {
		CMD_ERROR("ext enable not yet implemented (M4)");
		return 0;
	}
	if (sub && strcmp(sub, "disable") == 0) {
		CMD_ERROR("ext disable not yet implemented (M4)");
		return 0;
	}
	/* /ext alone → show tools */
	CMD_HEADER("registered tools (%d)", ctx->tools.count);
	for (int i = 0; i < ctx->tools.count; i++) {
		printf("  %-15s %s\n",
		       ctx->tools.entries[i].desc.name,
		       ctx->tools.entries[i].desc.desc);
	}
	if (ctx->tools.count == 0)
		printf("  (none)\n");
	return 0;
}

static int cmd_render(struct cli_context *ctx, int argc, char **argv)
{
	const char *path = cmd_arg(argc, argv, 1);
	if (!path) {
		CMD_ERROR("usage: /render <file_path>");
		return -EINVAL;
	}
	char *expanded = file_expand_path(path);
	if (!file_exists(expanded)) {
		CMD_ERROR("file not found: %s", expanded);
		free(expanded);
		return -ENOENT;
	}
	const char *ext = strrchr(expanded, '.');
	if (ext) ext++;
	if (ext && (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mov") == 0 ||
		    strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mkv") == 0 ||
		    strcasecmp(ext, "webm") == 0 || strcasecmp(ext, "flv") == 0)) {
		if (video_play(expanded, ctx->config.render.mpv_args) != 0) {
			CMD_ERROR("failed to play video: %s", expanded);
			free(expanded);
			return -EIO;
		}
		CMD_OK("video: %s", expanded);
	} else if (ext && (strcasecmp(ext, "png") == 0 || strcasecmp(ext, "jpg") == 0 ||
			   strcasecmp(ext, "jpeg") == 0 || strcasecmp(ext, "gif") == 0 ||
			   strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "bmp") == 0 ||
			   strcasecmp(ext, "tga") == 0 || strcasecmp(ext, "hdr") == 0)) {
		int w = 0, h = 0, ch = 0;
		if (!stbi_info(expanded, &w, &h, &ch)) {
			CMD_ERROR("not a valid image file: %s", expanded);
			free(expanded);
			return -EIO;
		}
		image_render_terminal(expanded);
		CMD_OK("image: %s (%dx%d)", expanded, w, h);
	} else {
		size_t len = 0;
		char *text = file_read_all(expanded, &len);
		if (!text) {
			CMD_ERROR("failed to read file: %s", expanded);
			free(expanded);
			return -EIO;
		}
		markdown_render_ansi(text);
		free(text);
	}
	free(expanded);
	return 0;
}

static int cmd_export_alias(struct cli_context *ctx, int argc, char **argv)
{
	printf("use /save [format] instead\n");
	return 0;
}

static int cmd_skill(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cmd_arg(argc, argv, 1);
	if (sub && strcmp(sub, "list") == 0) {
		CMD_HEADER("available skills (%d)", ctx->skills->count);
		for (int i = 0; i < ctx->skills->count; i++) {
			struct skill_entry *e = &ctx->skills->entries[i];
			const char *marker = e->activated ? ANSI_GREEN " *" ANSI_RESET : "";
			printf("  %-25s %s%s\n", e->fm.name, e->fm.description,
			       marker);
		}
		if (ctx->skills->count == 0)
			printf("  (none — install skills to ~/.morph/skills/ or ~/.agents/skills/)\n");
		else
			printf("  " ANSI_DIM "(%s* = activated)" ANSI_RESET "\n", ANSI_GREEN);
		return 0;
	}
	if (sub && strcmp(sub, "info") == 0) {
		const char *name = cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /skill info <name>");
			return -EINVAL;
		}
		struct skill_entry *e = skill_lookup(ctx->skills, name);
		if (!e) {
			CMD_ERROR("skill not found: %s", name);
			return -ENOENT;
		}
		printf("  %-15s %s\n", "Name", e->fm.name);
		printf("  %-15s %s\n", "Description", e->fm.description);
		printf("  %-15s %s\n", "Directory", e->skill_dir);
		printf("  %-15s %s\n", "Enabled", e->enabled ? "yes" : "no");
		printf("  %-15s %s\n", "Activated", e->activated ? "yes" : "no");
		if (e->fm.license[0])
			printf("  %-15s %s\n", "License", e->fm.license);
		if (e->fm.compatibility[0])
			printf("  %-15s %s\n", "Compatibility", e->fm.compatibility);
		if (e->fm.allowed_tools[0])
			printf("  %-15s %s\n", "Allowed tools", e->fm.allowed_tools);
		for (int i = 0; i < e->fm.metadata_count; i++)
			printf("  %-15s %s = %s\n", (i == 0) ? "Metadata" : "",
			       e->fm.metadata[i].key, e->fm.metadata[i].value);
		return 0;
	}
	if (sub && strcmp(sub, "activate") == 0) {
		const char *name = cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /skill activate <name>");
			return -EINVAL;
		}
		struct skill_entry *e = skill_lookup(ctx->skills, name);
		if (!e) {
			CMD_ERROR("skill not found: %s", name);
			return -ENOENT;
		}
		int rc = skill_activate(e);
		if (rc == 0 && e->activated)
			CMD_OK("skill '%s' activated", name);
		else if (rc < 0)
			CMD_ERROR("failed to activate skill '%s': %d", name, rc);
		else
			CMD_OK("skill '%s' already activated", name);
		return rc < 0 ? rc : 0;
	}
	if (sub && strcmp(sub, "deactivate") == 0) {
		const char *name = cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /skill deactivate <name>");
			return -EINVAL;
		}
		struct skill_entry *e = skill_lookup(ctx->skills, name);
		if (!e) {
			CMD_ERROR("skill not found: %s", name);
			return -ENOENT;
		}
		skill_deactivate(e);
		CMD_OK("skill '%s' deactivated", name);
		return 0;
	}
	CMD_HEADER("available skills (%d)", ctx->skills->count);
	for (int i = 0; i < ctx->skills->count; i++) {
		struct skill_entry *e = &ctx->skills->entries[i];
		const char *marker = e->activated ? ANSI_GREEN " *" ANSI_RESET : "";
		printf("  %-25s %s%s\n", e->fm.name, e->fm.description, marker);
	}
	if (ctx->skills->count == 0)
		printf("  (none — install skills to ~/.morph/skills/ or ~/.agents/skills/)\n");
	return 0;
}

/* ---- dispatch ---- */

static int cmd_dispatch(struct cli_context *ctx, const char *input)
{
	char *argv[32];
	int argc = argv_split(input, argv, 32);
	if (argc < 1)
		return -EINVAL;

	const struct cmd_entry *e = cmd_lookup(argv[0]);
	if (!e) {
		CMD_ERROR("unknown command: %s. Try /help", argv[0]);
		return -ENOENT;
	}
	return e->handler(ctx, argc, argv);
}

/* ---- cli_init ---- */

int cli_init(struct cli_context *ctx, const char *config_path)
{
	if (!ctx)
		return -EINVAL;
	memset(ctx, 0, sizeof(*ctx));
	config_set_defaults(&ctx->config);
	if (!config_path)
		config_path = default_config_path;
	char *expanded = file_expand_path(config_path);
	if (file_exists(expanded))
		config_load(&ctx->config, expanded);
	free(expanded);
	char *db_path = file_expand_path(default_db_path);
	char *db_dir = file_expand_path("~/.morph");
	file_ensure_dir(db_dir);
	free(db_dir);
	int rc = db_open(&ctx->database, db_path);
	free(db_path);
	if (rc < 0) {
		log_err("failed to open database");
		return rc;
	}
	db_init_schema(&ctx->database);
	tool_registry_init(&ctx->tools);
	ctx->tokenizer = tokenizer_create(ctx->config.models.text.model,
					  ctx->config.models.text.context_limit);
	if (!ctx->tokenizer) {
		log_err("failed to create tokenizer");
		db_close(&ctx->database);
		return -ENOMEM;
	}
	struct compress_config compress_cfg = {
		.max_context_tokens = ctx->config.models.text.context_limit,
		.max_history_rounds = ctx->config.context.keep_recent_rounds,
		.summarize_threshold_ratio = ctx->config.context.summarize_threshold_ratio,
		.compress_target_ratio = ctx->config.context.compress_target_ratio,
	};
	struct guardrail_config guardrail_cfg = {
		.enabled = ctx->config.react.guardrail_enabled,
		.max_retries = ctx->config.react.guardrail_max_retries,
		.min_tool_calls = ctx->config.react.guardrail_min_tool_calls,
		.must_have_output = ctx->config.react.guardrail_must_have_output,
		.max_empty_rounds = ctx->config.react.guardrail_max_empty_rounds,
	};
	ctx->react = react_context_create(&ctx->tools, ctx->tokenizer, &compress_cfg, &guardrail_cfg);
	if (!ctx->react) {
		log_err("failed to create react context");
		tokenizer_destroy(ctx->tokenizer);
		db_close(&ctx->database);
		return -ENOMEM;
	}
	ctx->react->step_timeout_seconds = ctx->config.react.step_timeout_seconds;
	ctx->react->tool_max_retries = ctx->config.react.tool_max_retries;
	ctx->react->max_iterations = ctx->config.react.max_iterations;

	if (ctx->config.prompt.system_prompt_file[0]) {
		char *exp = file_expand_path(ctx->config.prompt.system_prompt_file);
		if (exp) {
			char *content = file_read_all(exp, NULL);
			if (content) {
				size_t len = strlen(content);
				while (len > 0 && (content[len-1] == '\n' ||
				       content[len-1] == '\r' || content[len-1] == ' '))
					content[--len] = '\0';
				ctx->react->system_prompt = content;
				log_info("loaded system prompt: %s",
					 ctx->config.prompt.system_prompt_file);
			} else {
				log_warn("failed to read system prompt: %s",
					 ctx->config.prompt.system_prompt_file);
			}
			free(exp);
		}
	}

	if (ctx->config.prompt.system_prompt_dir[0]) {
		char *exp2 = file_expand_path(ctx->config.prompt.system_prompt_dir);
		if (exp2) {
			char **files = NULL;
			int nfiles = 0;
			if (file_list_files(exp2, &files, &nfiles) == 0) {
				for (int i = 0; i < nfiles; i++) {
					char full[4096];
					snprintf(full, sizeof(full), "%s/%s", exp2, files[i]);
					char *content = file_read_all(full, NULL);
					if (!content)
						continue;
					size_t clen = strlen(content);
					while (clen > 0 && (content[clen-1] == '\n' ||
					       content[clen-1] == '\r' || content[clen-1] == ' '))
						content[--clen] = '\0';
					if (!clen) {
						free(content);
						continue;
					}
					char *old = ctx->react->system_prompt;
					size_t old_len = old ? strlen(old) : 0;
					char *combined = malloc(old_len + 3 + clen + 1);
					if (combined) {
						if (old) {
							memcpy(combined, old, old_len);
							combined[old_len] = '\n';
							combined[old_len + 1] = '\n';
							memcpy(combined + old_len + 2, content, clen + 1);
						} else {
							memcpy(combined, content, clen + 1);
						}
						ctx->react->system_prompt = combined;
					}
					free(old);
					free(content);
				}
				file_free_list(files, nfiles);
				log_info("loaded %d prompt files from: %s",
					 nfiles, ctx->config.prompt.system_prompt_dir);
			}
			free(exp2);
		}
	}

	strncpy(ctx->current_session.name, ctx->config.general.default_session,
		sizeof(ctx->current_session.name) - 1);

	const char *api_key = NULL;
	if (ctx->config.models.text.api_key[0])
		api_key = ctx->config.models.text.api_key;
	else
		api_key = getenv(ctx->config.models.text.api_key_env);

struct model *llm = model_llm_create(
		ctx->config.models.text.provider,
		ctx->config.models.text.model,
		ctx->config.models.text.api_base,
		api_key ? api_key : "");
  ctx->llm = llm;
  ctx->react->llm_model = llm;
  if (llm) {
	  llm->timeout_seconds = ctx->config.models.text.timeout_seconds;
	  if (ctx->config.models.text.max_tokens > 0)
		  llm->max_tokens = ctx->config.models.text.max_tokens;
	  if (ctx->config.models.text.context_limit > 0)
		  llm->context_limit = ctx->config.models.text.context_limit;
  }

	text_gen_init(&ctx->tools, llm);
	log_info("registered text_gen tool");

	text_qa_init(&ctx->tools, llm);
	log_info("registered text_qa tool");

	const char *img_api_key = NULL;
	if (ctx->config.models.image.api_key[0])
		img_api_key = ctx->config.models.image.api_key;
	else
		img_api_key = getenv(ctx->config.models.image.api_key_env);
	struct model *img_llm = model_llm_create(
		ctx->config.models.image.provider,
		ctx->config.models.image.model,
		ctx->config.models.image.api_base[0] ?
			ctx->config.models.image.api_base : NULL,
		img_api_key ? img_api_key : "");
	ctx->img_llm = img_llm;
	img_gen_init(&ctx->tools, img_llm);
	log_info("registered img_gen tool");

	img_edit_init(&ctx->tools, llm);
	log_info("registered img_edit tool");

	img_info_init(&ctx->tools);
	log_info("registered img_info tool");

	file_read_init(&ctx->tools);
	log_info("registered file_read tool");

	file_list_init(&ctx->tools);
	log_info("registered file_list tool");

	file_info_init(&ctx->tools);
	log_info("registered file_info tool");

	bash_exec_init(&ctx->tools);
	log_info("registered bash_exec tool");

	img_resize_init(&ctx->tools);
	log_info("registered img_resize tool");

	img_convert_init(&ctx->tools);
	log_info("registered img_convert tool");

	const char *vid_api_key = NULL;
	if (ctx->config.models.video.api_key[0])
		vid_api_key = ctx->config.models.video.api_key;
	else
		vid_api_key = getenv(ctx->config.models.video.api_key_env);
	struct model *vid_llm = model_llm_create(
		ctx->config.models.video.provider,
		ctx->config.models.video.model,
		ctx->config.models.video.api_base[0] ?
			ctx->config.models.video.api_base : NULL,
		vid_api_key ? vid_api_key : "");
	ctx->vid_llm = vid_llm;
	vid_gen_init(&ctx->tools, vid_llm);
	log_info("registered vid_gen tool");

	/* Apply disabled tools from config */
	for (int i = 0; i < ctx->config.react.disabled_tools_count; i++) {
		tool_disable(&ctx->tools, ctx->config.react.disabled_tools[i]);
	}

	/* Auto-discover exts from exts/ directory */
	char exts_dir[512] = {0};
	char *exts_home = file_expand_path("~/.morph/exts");
	if (exts_home) {
		strncpy(exts_dir, exts_home, sizeof(exts_dir) - 1);
		free(exts_home);
	} else {
		strncpy(exts_dir, "exts", sizeof(exts_dir) - 1);
	}
	if (!file_exists(exts_dir))
		file_ensure_dir(exts_dir);
	char **ext_dirs = NULL;
	int ext_count = 0;
	if (file_list_dirs(exts_dir, &ext_dirs, &ext_count) == 0) {
		for (int i = 0; i < ext_count; i++) {
			char ed_path[1024];
			snprintf(ed_path, sizeof(ed_path), "%s/%s", exts_dir, ext_dirs[i]);
			struct ext ex;
			int rc2 = ext_load(&ex, ed_path);
			if (rc2 == 0 && ex.enabled) {
				struct ext *ex_ptr = malloc(sizeof(*ex_ptr));
				if (ex_ptr) {
					memcpy(ex_ptr, &ex, sizeof(ex));
					tool_register(&ctx->tools, ex.manifest.name,
						      ex.manifest.description,
						      ex.manifest.args_schema ?
						      ex.manifest.args_schema : "",
						      ext_run_wrapper, ex_ptr);
					log_info("registered ext: %s", ex.manifest.name);
				}
			} else {
				ext_unload(&ex);
			}
		}
		file_free_list(ext_dirs, ext_count);
	}

	ctx->skills = calloc(1, sizeof(*ctx->skills));
	if (!ctx->skills) {
		log_err("failed to allocate skill registry");
		return -ENOMEM;
	}
	skill_registry_init(ctx->skills);

	if (ctx->config.skill.dir[0]) {
		char *skill_dir = file_expand_path(ctx->config.skill.dir);
		if (skill_dir) {
			if (file_exists(skill_dir))
				skill_discover(ctx->skills, skill_dir);
			free(skill_dir);
		}
	} else {
		char *morph_skills = file_expand_path("~/.morph/skills");
		if (morph_skills) {
			if (!file_exists(morph_skills))
				file_ensure_dir(morph_skills);
			skill_discover(ctx->skills, morph_skills);
			free(morph_skills);
		}
		char *agents_skills = file_expand_path("~/.agents/skills");
		if (agents_skills) {
			if (!file_exists(agents_skills))
				file_ensure_dir(agents_skills);
			skill_discover(ctx->skills, agents_skills);
			free(agents_skills);
		}
	}

	if (ctx->skills->count > 0) {
		skill_activate_init(&ctx->tools, ctx->skills);
		log_info("registered activate_skill tool (%d skills discovered)",
			 ctx->skills->count);
	}

	ctx->react->skills = ctx->skills;

	rc = session_create(&ctx->database, ctx->current_session.name,
			    ctx->config.models.text.model, &ctx->current_session);
	if (rc == -EEXIST) {
		rc = session_get_by_name(&ctx->database, ctx->current_session.name,
					&ctx->current_session);
		if (rc < 0) {
			log_err("failed to get default session");
			return rc;
		}
		ctx->session_auto_named = 0;
		session_update_model(&ctx->database, ctx->current_session.id,
				    ctx->config.models.text.model);
		strncpy(ctx->current_session.model, ctx->config.models.text.model,
			sizeof(ctx->current_session.model) - 1);
		utf8_sanitize_inplace(ctx->current_session.name);
		session_load_history(ctx);
	} else {
		ctx->session_auto_named = 0;
	}

	session_ensure_display_id(&ctx->database, &ctx->current_session);

	ctx->running = 1;
	ctx->streaming = 0;
	ctx->image_path[0] = '\0';
	log_info("cli initialized");

	spin_init(&ctx->spin, stdout);
	spin_set_cancel_flag(&ctx->spin, &react_sigint_flag);

	return 0;
}

/* ---- sigint ---- */

static volatile sig_atomic_t sigint_received = 0;

static void sigint_handler(int sig)
{
	(void)sig;
	react_sigint_flag = 1;
	sigint_received = 1;
	if (write(STDOUT_FILENO, "\n", 1) < 0) {
		/* ignore */
	}
}

#ifdef HAVE_READLINE

static char *cmd_completion_generator(const char *text, int state)
{
	static int idx;
	static int len;
	if (state == 0) {
		idx = 0;
		len = (int)strlen(text);
	}
	while (idx < num_commands) {
		const char *name = commands[idx].name;
		idx++;
		if (strncmp(name, text, (size_t)len) == 0)
			return strdup(name + 1); /* strip leading / */
	}
	return NULL;
}

static char **cmd_completion(const char *text, int start, int end)
{
	(void)end;
	if (start == 0 && text[0] == '/')
		rl_attempted_completion_function = NULL;
	if (start == 0)
		return rl_completion_matches(text, cmd_completion_generator);
	return NULL;
}

#endif

/* ---- cli_run ---- */

void cli_run(struct cli_context *ctx)
{
	if (!ctx)
		return;
	printf("morph v0.1  |  " ANSI_DIM "/help 查看命令" ANSI_RESET "\n\n");
	char line[8192];

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigint_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);

#ifdef HAVE_READLINE
	using_history();
	rl_attempted_completion_function = cmd_completion;
	while (ctx->running) {
		char prompt[512];
		snprintf(prompt, sizeof(prompt), ANSI_GREEN "[%s]" ANSI_RESET " $ ",
			 ctx->current_session.display_id);
		sigint_received = 0;
		char *input = readline(prompt);
		if (!input) {
			if (sigint_received) {
				sigint_received = 0;
				continue;
			}
			if (feof(stdin))
				break;
			clearerr(stdin);
			printf("\n");
			continue;
		}
		if (input[0] != '\0') {
			add_history(input);
			strncpy(line, input, sizeof(line) - 1);
			line[sizeof(line) - 1] = '\0';
			cli_handle_command(ctx, line);
		}
		free(input);
	}
#else
	while (ctx->running) {
		printf(ANSI_GREEN "[%s]" ANSI_RESET " $ ", ctx->current_session.display_id);
		fflush(stdout);
		sigint_received = 0;
		if (!fgets(line, sizeof(line), stdin)) {
			if (sigint_received) {
				sigint_received = 0;
				clearerr(stdin);
				continue;
			}
			if (feof(stdin))
				break;
			clearerr(stdin);
			continue;
		}
		line[strcspn(line, "\n")] = '\0';
		if (line[0] == '\0')
			continue;
		cli_handle_command(ctx, line);
	}
#endif
	signal(SIGINT, SIG_DFL);
}

/* ---- output_callback ---- */

static int is_image_ext(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (!ext) return 0;
	return (strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0 ||
		strcasecmp(ext, ".jpeg") == 0 || strcasecmp(ext, ".gif") == 0 ||
		strcasecmp(ext, ".webp") == 0 || strcasecmp(ext, ".bmp") == 0);
}

static int is_video_ext(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (!ext) return 0;
	return (strcasecmp(ext, ".mp4") == 0 || strcasecmp(ext, ".mov") == 0 ||
		strcasecmp(ext, ".avi") == 0 || strcasecmp(ext, ".mkv") == 0 ||
		strcasecmp(ext, ".webm") == 0);
}

static int is_media_path(const char *path)
{
	return is_image_ext(path) || is_video_ext(path);
}

static char *wrap_bare_media_paths(const char *content)
{
	if (!content || !*content)
		return NULL;

	size_t len = strlen(content);
	size_t cap = len * 3 + 1;
	char *out = malloc(cap);
	if (!out)
		return NULL;
	size_t olen = 0;

	const char *p = content;
	while (*p) {
		if (p[0] == '!' && p[1] == '[') {
			const char *close = strchr(p + 2, ')');
			if (close) {
				size_t chunk = (close - p) + 1;
				memcpy(out + olen, p, chunk);
				olen += chunk;
				p = close + 1;
				continue;
			}
		}
		if (p[0] == '[' && p[1] != ']') {
			const char *close = strchr(p + 1, ')');
			if (close) {
				size_t chunk = (close - p) + 1;
				memcpy(out + olen, p, chunk);
				olen += chunk;
				p = close + 1;
				continue;
			}
		}

		const char *path_start = NULL;
		const char *q = p;
		while (*q) {
			if ((q == content || q[-1] == ' ' || q[-1] == '\n' || q[-1] == '\r' || q[-1] == '\t' || q[-1] == ':' || q[-1] == '`') &&
			    (strncmp(q, "~/.morph/output/", 16) == 0 ||
			     strncmp(q, "/.morph/output/", 15) == 0)) {
				path_start = q;
				break;
			}
			q++;
		}

		if (!path_start) {
			out[olen++] = *p++;
			continue;
		}

		if (path_start > p) {
			size_t pre = path_start - p;
			memcpy(out + olen, p, pre);
			olen += pre;
		}

		const char *path_end = path_start;
		while (*path_end && *path_end != ' ' && *path_end != '\n' &&
		       *path_end != '\r' && *path_end != '\t' &&
		       *path_end != ')' && *path_end != ']' &&
		       *path_end != '`' && *path_end != '"' &&
		       *path_end != '\'' && *path_end != ',' &&
		       *path_end != ';' && *path_end != '\0')
			path_end++;

		size_t path_len = (size_t)(path_end - path_start);
		if (path_len > 0 && is_media_path(path_start)) {
			const char *prefix;
			const char *suffix;
			if (is_image_ext(path_start)) {
				prefix = "![image](";
				suffix = ")";
			} else {
				prefix = "[video](";
				suffix = ")";
			}
			size_t plen = strlen(prefix);
			size_t slen = strlen(suffix);
			while (olen + plen + path_len + slen + 1 >= cap) {
				cap *= 2;
				char *nb = realloc(out, cap);
				if (!nb) { free(out); return NULL; }
				out = nb;
			}
			memcpy(out + olen, prefix, plen);
			olen += plen;
			memcpy(out + olen, path_start, path_len);
			olen += path_len;
			memcpy(out + olen, suffix, slen);
			olen += slen;
		} else {
			memcpy(out + olen, path_start, path_len);
			olen += path_len;
		}
		p = path_end;
	}

	out[olen] = '\0';
	return out;
}

static void media_callback(const char *type, const char *path, void *user)
{
	if (strcmp(type, "image") == 0) {
		image_render_terminal(path);
	} else if (strcmp(type, "video") == 0) {
		struct cli_context *ctx = (struct cli_context *)user;
		const char *mpv = (ctx && ctx->config.render.mpv_args[0])
				  ? ctx->config.render.mpv_args : NULL;
		video_play(path, mpv);
	}
}

static int output_callback(enum react_step_type type, const char *content,
			   void *user_data)
{
	struct cli_context *ctx = user_data;

	switch (type) {
	case REACT_STEP_THOUGHT:
		if (content && *content) {
			if (!ctx->streaming) {
				ctx->streaming = 1;
				ctx->stream_buf[0] = '\0';
				ctx->stream_buf_len = 0;
				if (!ctx->spin.running) {
					spin_start(&ctx->spin, SPIN_STATE_THINKING,
						   "Thinking");
				}
			}
			size_t clen = strlen(content);
			size_t avail = sizeof(ctx->stream_buf) - ctx->stream_buf_len - 1;
			if (clen > avail) {
				size_t keep = sizeof(ctx->stream_buf) / 2;
				memmove(ctx->stream_buf, ctx->stream_buf + ctx->stream_buf_len - keep, keep);
				ctx->stream_buf_len = keep;
				avail = sizeof(ctx->stream_buf) - ctx->stream_buf_len - 1;
			}
			if (clen > avail)
				clen = avail;
			memcpy(ctx->stream_buf + ctx->stream_buf_len, content, clen);
			ctx->stream_buf_len += clen;
			ctx->stream_buf[ctx->stream_buf_len] = '\0';

			const char *last_nl = strrchr(ctx->stream_buf, '\n');
			const char *preview = last_nl ? last_nl + 1 : ctx->stream_buf;
			while (*preview == ' ' || *preview == '\t')
				preview++;
			size_t plen = strlen(preview);
			if (plen > 60)
				preview = preview + plen - 60;
			char sub[128];
			snprintf(sub, sizeof(sub), "%.60s", preview);
			spin_set_sub(&ctx->spin, sub);
		} else if (!ctx->streaming) {
			if (!ctx->spin.running) {
				spin_start(&ctx->spin, SPIN_STATE_THINKING, "Thinking");
			}
			ctx->streaming = 1;
			ctx->stream_buf[0] = '\0';
			ctx->stream_buf_len = 0;
		}
		break;
	case REACT_STEP_ACTION:
		if (ctx->streaming) {
			spin_set_sub(&ctx->spin, NULL);
			ctx->streaming = 0;
		}
		if (content && strncmp(content, "Executing ", 10) == 0) {
			if (ctx->spin.running) {
				char msg[256];
				snprintf(msg, sizeof(msg), "Running %s", content + 10);
				spin_update(&ctx->spin, msg);
			}
			break;
		}
		if (content && strstr(content, " completed")) {
			break;
		}
		{
			char tool_name[64] = {0};
			if (content) {
				const char *paren = strchr(content, '(');
				if (paren) {
					size_t nlen = (size_t)(paren - content);
					if (nlen >= sizeof(tool_name)) nlen = sizeof(tool_name) - 1;
					memcpy(tool_name, content, nlen);
					tool_name[nlen] = '\0';
				} else {
					snprintf(tool_name, sizeof(tool_name), "%s", content);
				}
			}
			if (!ctx->spin.running) {
				spin_start(&ctx->spin, SPIN_STATE_EXECUTING,
					   tool_name[0] ? tool_name : "Executing");
			} else {
				spin_update(&ctx->spin, tool_name[0] ? tool_name : "Executing");
			}
			if (content)
				spin_set_sub(&ctx->spin, content);
		}
		break;
	case REACT_STEP_OBSERVATION:
		if (ctx->streaming) {
			spin_set_sub(&ctx->spin, NULL);
			ctx->streaming = 0;
		}
		if (ctx->spin.running) {
			char msg[128] = {0};
			if (content && strncmp(content, "tool error:", 11) == 0) {
				snprintf(msg, sizeof(msg), "Tool execution failed");
				spin_stop(&ctx->spin, SPIN_STATE_ERROR, msg);
			} else if (content && strncmp(content, "image generated:", 15) == 0) {
				snprintf(msg, sizeof(msg), "Image generated");
				spin_stop(&ctx->spin, SPIN_STATE_COMPLETE, msg);
			} else if (content && strncmp(content, "video generated:", 16) == 0) {
				snprintf(msg, sizeof(msg), "Video generated");
				spin_stop(&ctx->spin, SPIN_STATE_COMPLETE, msg);
			} else {
				snprintf(msg, sizeof(msg), "Done");
				spin_stop(&ctx->spin, SPIN_STATE_COMPLETE, msg);
			}
		}
		fflush(stdout);
		break;
	case REACT_STEP_REFLECTION:
		if (ctx->streaming) {
			spin_set_sub(&ctx->spin, NULL);
			ctx->streaming = 0;
		}
		if (ctx->spin.running) {
			spin_update(&ctx->spin, "Guardrail check");
		}
		printf(ANSI_BOLD ANSI_CYAN "[Guardrail]" ANSI_RESET " %s\n",
		       content ? content : "");
		fflush(stdout);
		break;
	case REACT_STEP_FINAL:
		if (ctx->streaming) {
			spin_set_sub(&ctx->spin, NULL);
			ctx->streaming = 0;
		}
		if (ctx->spin.running) {
			char msg[128];
			if (content && *content) {
				snprintf(msg, sizeof(msg), "Done");
			} else {
				snprintf(msg, sizeof(msg), "No output");
			}
			spin_stop(&ctx->spin, SPIN_STATE_COMPLETE, msg);
			printf("\n");
		}
		if (content && *content) {
			char *wrapped = wrap_bare_media_paths(content);
			markdown_render_ansi_with_media(wrapped ? wrapped : content,
							media_callback, ctx);
			free(wrapped);
		} else {
			printf("\n");
		}

		printf("\n");
		fflush(stdout);
		break;
	}
	return 0;
}

/* ---- cli_handle_command ---- */

int cli_handle_command(struct cli_context *ctx, const char *input)
{
	if (!ctx || !input)
		return -EINVAL;

	if (input[0] == '/')
		return cmd_dispatch(ctx, input);

	char input_buf[8192];
	const char *effective_input = input;
	if (ctx->image_path[0]) {
		int n = snprintf(input_buf, sizeof(input_buf),
				 "[Image: %s]\n%s", ctx->image_path, input);
		if (n > 0 && (size_t)n < sizeof(input_buf))
			effective_input = input_buf;
		ctx->image_path[0] = '\0';
	}

	/* Auto-name session from first user input */
	if (!ctx->session_auto_named && input[0] != '/') {
		char title[48];
		size_t len = strlen(input);
		size_t max_bytes = sizeof(title) - 4;
		if (len > max_bytes) {
			size_t chop = utf8_truncate(input, max_bytes);
			memcpy(title, input, chop);
			title[chop] = '\0';
			strcat(title, "...");
		} else {
			memcpy(title, input, len);
			title[len] = '\0';
		}
		session_rename(&ctx->database, ctx->current_session.id, title);
		strncpy(ctx->current_session.name, title,
			sizeof(ctx->current_session.name) - 1);
		ctx->session_auto_named = 1;
	}

	sigint_received = 0;
	if (ctx->react)
		react_cancel(ctx->react);

	react_run(ctx->react, effective_input, output_callback, ctx);

	if (ctx->react && ctx->react->state == REACT_STATE_ABORT) {
		printf("\n" ANSI_YELLOW "[aborted] ReAct loop cancelled or timed out." ANSI_RESET "\n");
	}

	/* Persist trace to DB */
	if (ctx->react && ctx->react->steps) {
		cJSON *arr = cJSON_CreateArray();
		struct react_step *cur = ctx->react->steps;
		while (cur) {
			cJSON *obj = cJSON_CreateObject();
			cJSON_AddStringToObject(obj, "type", react_step_type_name(cur->type));
			if (cur->content)
				cJSON_AddStringToObject(obj, "content", cur->content);
			if (cur->tool_name)
				cJSON_AddStringToObject(obj, "tool_name", cur->tool_name);
			if (cur->tool_args)
				cJSON_AddStringToObject(obj, "tool_args", cur->tool_args);
			if (cur->tool_call_id)
				cJSON_AddStringToObject(obj, "tool_call_id", cur->tool_call_id);
			cJSON_AddItemToArray(arr, obj);
			cur = cur->next;
		}
		char *json = cJSON_PrintUnformatted(arr);
		int round_no = trace_get_next_round_no(&ctx->database,
						       ctx->current_session.id);
		int aborted = (ctx->react->state == REACT_STATE_ABORT) ? 1 : 0;
		trace_save(&ctx->database, ctx->current_session.id,
			   round_no, json, aborted);
		free(json);
		cJSON_Delete(arr);
	}

	int user_tokens = tokenizer_count(ctx->tokenizer, effective_input);
	message_add(&ctx->database, ctx->current_session.id, "user",
		    effective_input, user_tokens);
	session_update_tokens(&ctx->database, ctx->current_session.id, user_tokens);
	if (ctx->react && ctx->react->final_answer) {
		int asst_tokens = tokenizer_count(ctx->tokenizer, ctx->react->final_answer);
		message_add(&ctx->database, ctx->current_session.id, "assistant",
			    ctx->react->final_answer, asst_tokens);
		session_update_tokens(&ctx->database, ctx->current_session.id, asst_tokens);
	}
	ctx->streaming = 0;
	return 0;
}

/* ---- cli_shutdown ---- */

void cli_shutdown(struct cli_context *ctx)
{
	if (!ctx)
		return;
	if (ctx->react)
		react_context_destroy(ctx->react);
	if (ctx->tokenizer)
		tokenizer_destroy(ctx->tokenizer);
	for (int i = 0; i < ctx->tools.count; i++) {
		void *ud = ctx->tools.entries[i].user_data;
		if (ud) {
			ext_unload((struct ext *)ud);
			free(ud);
			ctx->tools.entries[i].user_data = NULL;
		}
	}
	if (ctx->llm)
		model_destroy(ctx->llm);
	if (ctx->img_llm)
		model_destroy(ctx->img_llm);
	if (ctx->vid_llm)
		model_destroy(ctx->vid_llm);
	skill_registry_cleanup(ctx->skills);
	free(ctx->skills);
	ctx->skills = NULL;
	db_close(&ctx->database);
	log_info("cli shutdown complete");
}
