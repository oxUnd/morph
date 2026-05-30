#include "cli.h"
#include "util/log.h"
#include "util/file.h"
#include "util/error.h"
#include "util/spin.h"
#include "util/arena.h"
#include "util/utf8.h"
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
#include "agent/tools/ask_user.h"
#include "agent/tools/img_annotate.h"
#include "agent/plan.h"
#include "agent/guardrail.h"
#include "agent/tool_context.h"
#include "agent/tools/plan.h"
#include "mcp/mcp.h"
#include "db/database.h"
#include "agent/memory.h"
#include "config.h"
#include "models/image_gen.h"
#include "models/video_gen.h"
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

static enum hitl_verdict hitl_approval_callback(const char *tool_name,
						const char *tool_args,
						void *user_data);

static enum command_verdict command_approval_callback(const char *command,
						      const char *cwd,
						      void *user_data);

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

static struct memory_options cli_memory_options(const struct cli_context *ctx)
{
	struct memory_options opts;

	memset(&opts, 0, sizeof(opts));
	if (!ctx)
		return opts;
	opts.enabled = ctx->config.memory.enabled;
	opts.hot_path_enabled = ctx->config.memory.hot_path_enabled;
	opts.cold_path_enabled = ctx->config.memory.cold_path_enabled;
	opts.llm_extract_enabled = ctx->config.memory.llm_extract_enabled;
	opts.max_facts = ctx->config.memory.max_facts;
	opts.max_episodes = ctx->config.memory.max_episodes;
	opts.max_procedures = ctx->config.memory.max_procedures;
	opts.max_context_chars = ctx->config.memory.max_context_chars;
	return opts;
}

static void cli_refresh_memory_context(struct cli_context *ctx,
				       const char *query)
{
	struct memory_options opts;
	char *memory_ctx;

	if (!ctx || !ctx->react)
		return;
	opts = cli_memory_options(ctx);
	memory_ctx = memory_build_context(&ctx->database, ctx->current_session.id,
					  query, &opts);
	react_set_memory_context(ctx->react, memory_ctx);
	free(memory_ctx);
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
static int cmd_mcp(struct cli_context *ctx, int argc, char **argv);
static int cmd_memory(struct cli_context *ctx, int argc, char **argv);
static int cmd_export_alias(struct cli_context *ctx, int argc, char **argv);
static int cmd_clear(struct cli_context *ctx, int argc, char **argv);

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
	{ "/switch",  cmd_switch,  "Switch to another session",         "/switch <name|id|^N>" },
	{ "/s",       cmd_switch,  "Alias for /switch",                 "/s <name|id|^N>" },
	{ "/list",    cmd_list,    "List sessions",                     "/list [n|query|--all]" },
	{ "/ls",      cmd_list,    "Alias for /list",                   "/ls [n|query|--all]" },
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
	{ "/mcp",     cmd_mcp,     "List or manage MCP servers",        "/mcp list" },
	{ "/memory",  cmd_memory,  "Show or clear long-term memory",    "/memory [show|clear] [all|facts|episodes|procedures]" },
	{ "/mem",     cmd_memory,  "Alias for /memory",                 "/mem [show|clear] [all|facts|episodes|procedures]" },
	{ "/render",  cmd_render,  "Render a file (image/video/markdown)", "/render <file_path>" },
	{ "/r",       cmd_render,  "Alias for /render",                  "/r <file_path>" },
	{ "/export",  cmd_export_alias, "Alias for /save",              "/export <format>" },
	{ "/clear",   cmd_clear,   "Clear the terminal screen",         "/clear" },
	{ "/cl",      cmd_clear,   "Alias for /clear",                  "/cl" },
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
	char name_buf[256];
	const char *name = cmd_arg(argc, argv, 1);
	int auto_named = 0;
	if (!name) {
		auto_named = 1;
		snprintf(name_buf, sizeof(name_buf), "new_%lld",
			 (long long)time(NULL));
		name = name_buf;
	}
	struct session s;
	int rc = session_create(&ctx->database, name,
				ctx->config.models.text.model, &s);
	if (rc == 0) {
		ctx->current_session = s;
		utf8_sanitize_inplace(ctx->current_session.name);
		session_load_history(ctx);
		ctx->session_auto_named = !auto_named;
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
		CMD_ERROR("usage: /switch <name|id|^N>");
		return -EINVAL;
	}

	struct session s;
	int rc = -1;

	if (name[0] == '^') {
		int idx = 1;
		if (name[1] != '\0') {
			char *end;
			errno = 0;
			long n = strtol(name + 1, &end, 10);
			if (*end != '\0' || errno != 0 || n < 1) {
				CMD_ERROR("session not found: %s", name);
				return -ENOENT;
			}
			idx = (int)n;
		}
		struct session *list;
		int count = 0;
		session_list(&ctx->database, &list, &count, 0, NULL);
		int picked = 0;
		for (int i = 0; i < count; i++) {
			if (list[i].id == ctx->current_session.id)
				continue;
			picked++;
			if (picked == idx) {
				s = list[i];
				rc = 0;
				break;
			}
		}
		free(list);
	} else {
		rc = session_get_by_name(&ctx->database, name, &s);
		if (rc < 0)
			rc = session_get_by_display_id(&ctx->database, name, &s);
		if (rc < 0) {
			char *end;
			errno = 0;
			long id = strtol(name, &end, 10);
			if (*end == '\0' && errno == 0)
				rc = session_get_by_id(&ctx->database,
						       (int64_t)id, &s);
		}
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

#define SESSION_LIST_DEFAULT 20

static int cmd_list(struct cli_context *ctx, int argc, char **argv)
{
	int limit = SESSION_LIST_DEFAULT;
	const char *filter = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--all") == 0)
			limit = 0;
		else if (argv[i][0] && isdigit((unsigned char)argv[i][0]))
			limit = atoi(argv[i]);
		else
			filter = argv[i];
	}
	if (limit < 0)
		limit = 0;

	struct session *list;
	int count = 0;
	session_list(&ctx->database, &list, &count, limit, filter);

	int total = session_count(&ctx->database);

	if (filter && filter[0]) {
		if (limit > 0 && count < total)
			printf(ANSI_BOLD ANSI_CYAN
			       "--- sessions (%d of %d, \""
			       ANSI_YELLOW "%s"
			       ANSI_CYAN "\")"
			       ANSI_RESET "\n",
			       count, total, filter);
		else
			printf(ANSI_BOLD ANSI_CYAN
			       "--- sessions (%d of %d, \""
			       ANSI_YELLOW "%s"
			       ANSI_CYAN "\")"
			       ANSI_RESET "\n",
			       count, total, filter);
	} else if (limit > 0 && count < total) {
		printf(ANSI_BOLD ANSI_CYAN
		       "--- sessions (%d of %d, use --all for more)"
		       ANSI_RESET "\n",
		       count, total);
	} else {
		printf(ANSI_BOLD ANSI_CYAN
		       "--- sessions (%d)"
		       ANSI_RESET "\n",
		       count);
	}

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
		ctx->current_session.name[sizeof(ctx->current_session.name) - 1] = '\0';
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
		errno = 0;
		id = strtol(name, &end, 10);
		if (*end != '\0' || errno != 0)
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
		ctx->current_session.model[sizeof(ctx->current_session.model) - 1] = '\0';
		session_update_model(&ctx->database, ctx->current_session.id, name);
		if (ctx->llm) {
			strncpy(ctx->llm->model_id, name,
				sizeof(ctx->llm->model_id) - 1);
			ctx->llm->model_id[sizeof(ctx->llm->model_id) - 1] = '\0';
		}
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
	if (!cJSON_IsArray(arr)) {
		cJSON_Delete(arr);
		return NULL;
	}
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
		char *args_buf = NULL;
		if (cJSON_IsString(tool_args) && tool_args->valuestring) {
			const char *tn = cJSON_IsString(tool_name) ? tool_name->valuestring : "";
			size_t ab_len = strlen(tn) + strlen(tool_args->valuestring) + 4;
			args_buf = malloc(ab_len);
			if (args_buf)
				snprintf(args_buf, ab_len, "%s(%s)", tn, tool_args->valuestring);
		}
		struct react_step *s = react_step_create(
			arena,
			type,
			cJSON_IsString(content) ? content->valuestring : NULL,
			cJSON_IsString(tool_name) ? tool_name->valuestring : NULL,
			args_buf ? args_buf : NULL,
			cJSON_IsString(tool_call_id) ? tool_call_id->valuestring : NULL);
		if (s) {
			tail->next = s;
			tail = s;
		}
		free(args_buf);
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
	if (!ids) {
		message_free_list(msgs);
		return -ENOMEM;
	}
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
		CMD_ERROR("compression failed: %s", morph_strerror(rc));
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
	printf("  log_file = %s\n", ctx->config.general.log_file);
	printf(ANSI_BOLD "[model.text]" ANSI_RESET "\n");
	printf("  provider = %s\n", ctx->config.models.text.provider);
	printf("  model = %s\n", ctx->config.models.text.model);
	printf("  api_base = %s\n", ctx->config.models.text.api_base);
	printf("  context_limit = %d\n", ctx->config.models.text.context_limit);
	printf("  max_tokens = %d\n", ctx->config.models.text.max_tokens);
	printf("  timeout_seconds = %d\n",
	       ctx->config.models.text.timeout_seconds);
	printf(ANSI_BOLD "[model.image]" ANSI_RESET "\n");
	printf("  provider = %s\n", ctx->config.models.image.provider);
	printf("  model = %s\n", ctx->config.models.image.model);
	printf("  api_base = %s\n", ctx->config.models.image.api_base);
	printf("  context_limit = %d\n",
	       ctx->config.models.image.context_limit);
	printf(ANSI_BOLD "[model.video]" ANSI_RESET "\n");
	printf("  provider = %s\n", ctx->config.models.video.provider);
	printf("  model = %s\n", ctx->config.models.video.model);
	printf("  api_base = %s\n", ctx->config.models.video.api_base);
	printf("  context_limit = %d\n",
	       ctx->config.models.video.context_limit);
	printf(ANSI_BOLD "[react]" ANSI_RESET "\n");
	printf("  max_iterations = %d\n", ctx->config.react.max_iterations);
	printf("  step_timeout = %d\n",
	       ctx->config.react.step_timeout_seconds);
	printf("  tool_max_retries = %d\n", ctx->config.react.tool_max_retries);
	printf("  guardrail_enabled = %d\n",
	       ctx->config.react.guardrail_enabled);
	printf("  guardrail_max_retries = %d\n",
	       ctx->config.react.guardrail_max_retries);
	printf("  guardrail_max_empty_rounds = %d\n",
	       ctx->config.react.guardrail_max_empty_rounds);
	printf("  guardrail_llm_model = %s\n",
	       ctx->config.react.guardrail_llm_model);
	printf("  hitl_enabled = %d\n", ctx->config.react.hitl_enabled);
	printf("  hitl_auto_approve_readonly = %d\n",
	       ctx->config.react.hitl_auto_approve_readonly);
	printf("  bash_exec_enabled = %d\n",
	       ctx->config.react.bash_exec_enabled);
	printf("  bash_exec_default_timeout = %d\n",
	       ctx->config.react.bash_exec_default_timeout);
	if (ctx->config.react.disabled_tools_count > 0) {
		printf("  disabled_tools =");
		for (int i = 0; i < ctx->config.react.disabled_tools_count; i++)
			printf(" %s", ctx->config.react.disabled_tools[i]);
		printf("\n");
	}
	if (ctx->config.react.hitl_tools_count > 0) {
		printf("  hitl_tools =");
		for (int i = 0; i < ctx->config.react.hitl_tools_count; i++)
			printf(" %s", ctx->config.react.hitl_tools[i]);
		printf("\n");
	}
	if (ctx->config.react.bash_exec_allowed_commands_count > 0) {
		printf("  bash_exec_allowed_commands =");
		for (int i = 0;
		     i < ctx->config.react.bash_exec_allowed_commands_count;
		     i++)
			printf(" %s",
			       ctx->config.react.bash_exec_allowed_commands[i]);
		printf("\n");
	}
	if (ctx->config.react.bash_exec_allowed_cwds_count > 0) {
		printf("  bash_exec_allowed_cwds =");
		for (int i = 0;
		     i < ctx->config.react.bash_exec_allowed_cwds_count; i++)
			printf(" %s",
			       ctx->config.react.bash_exec_allowed_cwds[i]);
		printf("\n");
	}
	printf(ANSI_BOLD "[context]" ANSI_RESET "\n");
	printf("  threshold = %.1f\n",
	       ctx->config.context.summarize_threshold_ratio);
	printf("  target = %.1f\n",
	       ctx->config.context.compress_target_ratio);
	printf("  keep_rounds = %d\n",
	       ctx->config.context.keep_recent_rounds);
	printf(ANSI_BOLD "[memory]" ANSI_RESET "\n");
	printf("  enabled = %d\n", ctx->config.memory.enabled);
	printf("  hot_path_enabled = %d\n",
	       ctx->config.memory.hot_path_enabled);
	printf("  cold_path_enabled = %d\n",
	       ctx->config.memory.cold_path_enabled);
	printf("  llm_extract_enabled = %d\n",
	       ctx->config.memory.llm_extract_enabled);
	printf("  max_facts = %d\n", ctx->config.memory.max_facts);
	printf("  max_episodes = %d\n", ctx->config.memory.max_episodes);
	printf("  max_procedures = %d\n", ctx->config.memory.max_procedures);
	printf("  max_context_chars = %d\n",
	       ctx->config.memory.max_context_chars);
	printf(ANSI_BOLD "[render]" ANSI_RESET "\n");
	printf("  prefer_image_protocol = %s\n",
	       ctx->config.render.prefer_image_protocol);
	printf("  mpv_args = %s\n", ctx->config.render.mpv_args);
	printf(ANSI_BOLD "[ext]" ANSI_RESET "\n");
	printf("  dir = %s\n", ctx->config.ext.dir);
	printf("  default_max_memory_mb = %d\n",
	       ctx->config.ext.default_max_memory_mb);
	printf("  default_max_cpu_seconds = %d\n",
	       ctx->config.ext.default_max_cpu_seconds);
	printf(ANSI_BOLD "[prompt]" ANSI_RESET "\n");
	printf("  system_prompt_file = %s\n",
	       ctx->config.prompt.system_prompt_file);
	printf("  system_prompt_dir = %s\n",
	       ctx->config.prompt.system_prompt_dir);
	printf(ANSI_BOLD "[skill]" ANSI_RESET "\n");
	printf("  dir = %s\n", ctx->config.skill.dir);
	printf(ANSI_BOLD "[mcp]" ANSI_RESET "\n");
	printf("  server_count = %d\n", ctx->config.mcp.server_count);
	for (int i = 0; i < ctx->config.mcp.server_count; i++) {
		struct config_mcp_server *s = &ctx->config.mcp.servers[i];
		printf("  [[mcp.servers.%d]]\n", i);
		printf("    name = %s\n", s->name);
		printf("    transport = %s\n", s->transport);
		if (strcmp(s->transport, "stdio") == 0) {
			printf("    command = %s\n", s->command);
			printf("    args_count = %d\n", s->args_count);
		} else {
			printf("    http_url = %s\n", s->http_url);
		}
		printf("    auto_connect = %d\n", s->auto_connect);
	}
	return 0;
}

static const char *memory_scope_display(enum memory_clear_scope scope)
{
	switch (scope) {
	case MEMORY_CLEAR_ALL:
		return "all";
	case MEMORY_CLEAR_FACTS:
		return "facts";
	case MEMORY_CLEAR_EPISODES:
		return "episodes";
	case MEMORY_CLEAR_PROCEDURES:
		return "procedures";
	default:
		return "unknown";
	}
}

static int memory_parse_scope(const char *name, enum memory_clear_scope *scope)
{
	if (!scope)
		return -EINVAL;
	if (!name || strcmp(name, "all") == 0) {
		*scope = MEMORY_CLEAR_ALL;
		return 0;
	}
	if (strcmp(name, "facts") == 0) {
		*scope = MEMORY_CLEAR_FACTS;
		return 0;
	}
	if (strcmp(name, "episodes") == 0) {
		*scope = MEMORY_CLEAR_EPISODES;
		return 0;
	}
	if (strcmp(name, "procedures") == 0 ||
	    strcmp(name, "rules") == 0) {
		*scope = MEMORY_CLEAR_PROCEDURES;
		return 0;
	}
	return -EINVAL;
}

static int cmd_memory(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cmd_arg(argc, argv, 1);

	if (!sub || strcmp(sub, "show") == 0 || strcmp(sub, "view") == 0) {
		/*
		 * Pass 0 to render every stored episode/change so /mem
		 * gives the user the full picture; max_episodes is only
		 * a hint for the React-loop context window.
		 */
		char *rendered = memory_render_session(
			&ctx->database, ctx->current_session.id, 0);
		CMD_HEADER("memory (%s)", ctx->current_session.name);
		printf("%s\n", rendered ? rendered :
		       "No long-term memory stored for this session.");
		free(rendered);
		return 0;
	}

	if (strcmp(sub, "clear") == 0) {
		enum memory_clear_scope scope = MEMORY_CLEAR_ALL;
		const char *target = cmd_arg(argc, argv, 2);
		if (memory_parse_scope(target, &scope) != 0) {
			CMD_ERROR("usage: /memory clear [all|facts|episodes|procedures]");
			return -EINVAL;
		}
		if (memory_clear(&ctx->database, ctx->current_session.id, scope) != 0) {
			CMD_ERROR("failed to clear memory");
			return -EIO;
		}
		if (ctx->react)
			react_set_memory_context(ctx->react, NULL);
		CMD_OK("cleared %s memory for session: %s",
		       memory_scope_display(scope), ctx->current_session.name);
		return 0;
	}

	CMD_ERROR("usage: /memory [show|clear] [all|facts|episodes|procedures]");
	return -EINVAL;
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
		MORPH_RETURN(MORPH_ERR_FORMAT);
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
			MORPH_RETURN(MORPH_ERR_FORMAT);
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

static int cmd_clear(struct cli_context *ctx, int argc, char **argv)
{
	(void)ctx;
	(void)argc;
	(void)argv;
	printf("\033[2J\033[H");
	fflush(stdout);
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
			CMD_ERROR("failed to activate skill '%s': %s", name, morph_strerror(rc));
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

/* ---- mcp command ---- */

static int cmd_mcp(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cmd_arg(argc, argv, 1);

	if (!sub || strcmp(sub, "list") == 0 || strcmp(sub, "status") == 0) {
		CMD_HEADER("MCP servers (%d)", ctx->mcp.count);
		if (ctx->mcp.count == 0) {
			printf("  (none — add servers to config.toml under [mcp.servers])\n");
			return 0;
		}
		for (int i = 0; i < ctx->mcp.count; i++) {
			struct mcp_client *mc = ctx->mcp.servers[i];
			const char *status = mc->connected ?
				ANSI_GREEN "connected" ANSI_RESET :
				ANSI_YELLOW "disconnected" ANSI_RESET;
			const char *transport = mc->config.transport ==
				MCP_TRANSPORT_STDIO ? "stdio" : "http";

			printf("  %s%-20s%s  [%s]  %s\n",
				ANSI_BOLD, mc->config.name, ANSI_RESET,
				transport, status);
			if (mc->connected) {
				printf("    server: %s v%s | proto: %s\n",
					mc->server_name, mc->server_version,
					mc->negotiated_version);
				printf("    tools: %-3s  resources: %-3s  prompts: %-3s\n",
					mc->supports_tools ? "yes" : "no",
					mc->supports_resources ? "yes" : "no",
					mc->supports_prompts ? "yes" : "no");
			}
		}
		return 0;
	}

	if (strcmp(sub, "tools") == 0) {
		const char *name = cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp tools <server_name>");
			return -EINVAL;
		}
		struct mcp_client *mc = mcp_registry_get(&ctx->mcp, name);
		if (!mc) {
			CMD_ERROR("MCP server not found: %s", name);
			return -ENOENT;
		}
		int rc = mcp_ensure_connected(mc);
		if (rc < 0) {
			CMD_ERROR("failed to connect to '%s': %s", name, morph_strerror(rc));
			return rc;
		}
		struct mcp_tool_desc *tools = NULL;
		int count = 0;
		struct arena *arena = arena_create(0);
		if (!arena) {
			CMD_ERROR("failed to create arena");
			return -ENOMEM;
		}
		rc = mcp_list_tools(mc, arena, &tools, &count);
		if (rc < 0) {
			CMD_ERROR("failed to list tools: %s", morph_strerror(rc));
			arena_destroy(arena);
			return rc;
		}
		CMD_HEADER("MCP tools for '%s' (%d)", name, count);
		for (int i = 0; i < count; i++) {
			printf("  %-30s %s\n", tools[i].name, tools[i].description);
		}
		arena_destroy(arena);
		return 0;
	}

	if (strcmp(sub, "resources") == 0) {
		const char *name = cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp resources <server_name>");
			return -EINVAL;
		}
		struct mcp_client *mc = mcp_registry_get(&ctx->mcp, name);
		if (!mc) {
			CMD_ERROR("MCP server not found: %s", name);
			return -ENOENT;
		}
		int rc = mcp_ensure_connected(mc);
		if (rc < 0) {
			CMD_ERROR("failed to connect to '%s': %s", name, morph_strerror(rc));
			return rc;
		}
		struct mcp_resource_desc *res = NULL;
		int count = 0;
		struct arena *arena = arena_create(0);
		if (!arena) {
			CMD_ERROR("failed to create arena");
			return -ENOMEM;
		}
		rc = mcp_list_resources(mc, arena, &res, &count);
		if (rc < 0) {
			CMD_ERROR("failed to list resources: %s", morph_strerror(rc));
			arena_destroy(arena);
			return rc;
		}
		CMD_HEADER("MCP resources for '%s' (%d)", name, count);
		for (int i = 0; i < count; i++) {
			printf("  %-30s %s\n", res[i].name, res[i].description);
			printf("    uri: %s\n", res[i].uri);
		}
		arena_destroy(arena);
		return 0;
	}

	if (strcmp(sub, "prompts") == 0) {
		const char *name = cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp prompts <server_name>");
			return -EINVAL;
		}
		struct mcp_client *mc = mcp_registry_get(&ctx->mcp, name);
		if (!mc) {
			CMD_ERROR("MCP server not found: %s", name);
			return -ENOENT;
		}
		int rc = mcp_ensure_connected(mc);
		if (rc < 0) {
			CMD_ERROR("failed to connect to '%s': %s", name, morph_strerror(rc));
			return rc;
		}
		struct mcp_prompt_desc *prompts = NULL;
		int count = 0;
		struct arena *arena = arena_create(0);
		if (!arena) {
			CMD_ERROR("failed to create arena");
			return -ENOMEM;
		}
		rc = mcp_list_prompts(mc, arena, &prompts, &count);
		if (rc < 0) {
			CMD_ERROR("failed to list prompts: %s", morph_strerror(rc));
			arena_destroy(arena);
			return rc;
		}
		CMD_HEADER("MCP prompts for '%s' (%d)", name, count);
		for (int i = 0; i < count; i++) {
			printf("  %-30s %s\n", prompts[i].name, prompts[i].description);
		}
		arena_destroy(arena);
		return 0;
	}

	if (strcmp(sub, "connect") == 0) {
		const char *name = cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp connect <server_name>");
			return -EINVAL;
		}
		struct mcp_client *mc = mcp_registry_get(&ctx->mcp, name);
		if (!mc) {
			CMD_ERROR("MCP server not found: %s", name);
			return -ENOENT;
		}
		if (mc->connected) {
			CMD_OK("MCP server '%s' already connected", name);
			return 0;
		}
		int rc = mcp_ensure_connected(mc);
		if (rc < 0) {
			CMD_ERROR("failed to connect to '%s': %s", name, morph_strerror(rc));
			return rc;
		}
		mcp_register_server_tools(mc, &ctx->tools);
		mcp_register_server_resources(mc, &ctx->tools);
		mcp_register_server_prompts(mc, &ctx->tools);
		CMD_OK("MCP server '%s' connected", name);
		return 0;
	}

	if (strcmp(sub, "disconnect") == 0) {
		const char *name = cmd_arg(argc, argv, 2);
		if (!name) {
			CMD_ERROR("usage: /mcp disconnect <server_name>");
			return -EINVAL;
		}
		struct mcp_client *mc = mcp_registry_get(&ctx->mcp, name);
		if (!mc) {
			CMD_ERROR("MCP server not found: %s", name);
			return -ENOENT;
		}
		mcp_disconnect(mc);
		CMD_OK("MCP server '%s' disconnected", name);
		return 0;
	}

	CMD_ERROR("unknown MCP subcommand: %s. Try: list, tools, resources, prompts, connect, disconnect", sub);
	return -EINVAL;
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


/* ---- cli_init helpers ---- */

struct auto_connect_work {
	struct mcp_client *client;
	struct tool_registry *tools;
	int result;
	int done;
	pthread_mutex_t lock;
	pthread_cond_t cond;
};

static void *auto_connect_thread(void *arg)
{
	struct auto_connect_work *w = arg;
	int rc = mcp_ensure_connected(w->client);
	if (rc == 0) {
		mcp_register_server_tools(w->client, w->tools);
		mcp_register_server_resources(w->client, w->tools);
		mcp_register_server_prompts(w->client, w->tools);
	}
	pthread_mutex_lock(&w->lock);
	w->result = rc;
	w->done = 1;
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->lock);
	return NULL;
}

static int cli_ask_user_callback(const char *question,
				  const char *const *choices,
				  int choices_count,
				  char **answer,
				  void *user_data);

/*
 * Load configuration from TOML file and set defaults.
 * ctx - CLI context to configure.
 * config_path - Path to config file, or NULL for default.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int cli_init_config(struct cli_context *ctx, const char *config_path)
{
	config_set_defaults(&ctx->config);
	if (!config_path)
		config_path = default_config_path;
	char *expanded = file_expand_path(config_path);
	if (file_exists(expanded))
		config_load(&ctx->config, expanded);
	free(expanded);
	return 0;
}

/*
 * Open the database and initialize its schema.
 * ctx - CLI context with config already loaded.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int cli_init_database(struct cli_context *ctx)
{
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
	return 0;
}

/*
 * Create tokenizer, react context, and LLM/image/video models.
 * Configures compress, guardrail, HITL, and system prompts.
 * ctx - CLI context with config and database initialized.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int cli_init_models(struct cli_context *ctx)
{
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
		.max_empty_rounds = ctx->config.react.guardrail_max_empty_rounds,
	};
	ctx->react = react_context_create(&ctx->tools, ctx->tokenizer,
					  &compress_cfg, &guardrail_cfg);
	if (!ctx->react) {
		log_err("failed to create react context");
		tokenizer_destroy(ctx->tokenizer);
		db_close(&ctx->database);
		return -ENOMEM;
	}
	ctx->react->step_timeout_seconds = ctx->config.react.step_timeout_seconds;
	ctx->react->tool_max_retries = ctx->config.react.tool_max_retries;
	ctx->react->max_iterations = ctx->config.react.max_iterations;
	bash_exec_set_default_timeout(ctx->config.react.bash_exec_default_timeout);
	ctx->react->hitl.enabled = ctx->config.react.hitl_enabled;
	ctx->react->hitl.auto_approve_readonly = ctx->config.react.hitl_auto_approve_readonly;
	ctx->react->hitl.tools_count = ctx->config.react.hitl_tools_count;
	for (int i = 0; i < ctx->config.react.hitl_tools_count; i++)
		strncpy(ctx->react->hitl.tools[i], ctx->config.react.hitl_tools[i],
			HITL_TOOL_NAME_MAX - 1);
	if (ctx->react->hitl.enabled) {
		ctx->react->hitl.approval_cb = hitl_approval_callback;
		ctx->react->hitl.approval_user_data = ctx;
	}

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
	ctx->current_session.name[sizeof(ctx->current_session.name) - 1] = '\0';

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
	/* Wire the chat LLM into the memory subsystem so cold-path
	 * consolidation can use LLM-driven extraction. */
	memory_set_llm(llm);

	for (int i = 0; i < ctx->config.react.guardrail_llm_rule_count; i++) {
		struct config_guardrail_llm_rule *cr =
			&ctx->config.react.guardrail_llm_rules[i];
		enum guardrail_hook hook = GUARDRAIL_HOOK_OUTPUT;
		if (strcmp(cr->hook, "input") == 0)
			hook = GUARDRAIL_HOOK_INPUT;
		guardrail_rule_register(&ctx->react->guardrail, cr->name,
			hook, GUARDRAIL_RULE_LLM, NULL,
			cr->description, NULL, cr->action_text);
	}
	for (int i = 0; i < ctx->config.react.guardrail_ext_rule_count; i++) {
		struct config_guardrail_ext_rule *cr =
			&ctx->config.react.guardrail_ext_rules[i];
		enum guardrail_hook hook = GUARDRAIL_HOOK_OUTPUT;
		if (strcmp(cr->hook, "input") == 0)
			hook = GUARDRAIL_HOOK_INPUT;
		guardrail_rule_register(&ctx->react->guardrail, cr->name,
			hook, GUARDRAIL_RULE_EXT, NULL,
			cr->ext_type[0] == '\0' || strcmp(cr->ext_type, "exec") == 0
				? NULL : cr->ext_type,
			cr->ext_entry, cr->action_text);
		if (strcmp(cr->ext_type, "so") == 0) {
			struct guardrail_rule *r =
				guardrail_rule_lookup(&ctx->react->guardrail, cr->name);
			if (r) {
				r->ext_type = GUARDRAIL_EXT_SO;
				guardrail_ext_so_load(r);
			}
		}
	}
	if (ctx->config.react.guardrail_llm_model[0] && llm)
		guardrail_set_llm(&ctx->react->guardrail, llm);
	else if (llm)
		guardrail_set_llm(&ctx->react->guardrail, llm);
	for (int i = 0; i < ctx->config.react.guardrail_disabled_rule_count; i++)
		guardrail_rule_disable(&ctx->react->guardrail,
			ctx->config.react.guardrail_disabled_rules[i]);

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

	return 0;
}

/*
 * Register all built-in tools and configure tool flags.
 * Includes text_gen, text_qa, img_*, vid_*, file_*, bash_exec,
 * skill, plan, and ask_user tools.
 * ctx - CLI context with models and react context initialized.
 *
 * Returns 0 on success, negative errno on failure.
 */
static void cli_img_annotate_pause(void *user_data)
{
	struct cli_context *ctx = user_data;
	if (ctx)
		spin_pause(&ctx->spin);
}

static void cli_img_annotate_resume(void *user_data)
{
	struct cli_context *ctx = user_data;
	if (ctx) {
		fflush(stdout);
		spin_resume(&ctx->spin);
	}
}

static enum write_verdict write_approval_callback(const char *path,
						   const char *output_dir,
						   void *user_data);

static int cli_init_tools(struct cli_context *ctx)
{
	int rc = 0;

	ctx->tctx = tool_context_create(ctx->config.general.output_dir);
	if (!ctx->tctx) {
		log_err("failed to create tool context");
		return -ENOMEM;
	}
	ctx->tctx->approval_fn = write_approval_callback;
	ctx->tctx->approval_user_data = ctx;

	text_gen_init(&ctx->tools, ctx->llm);
	log_info("registered text_gen tool");

	text_qa_init(&ctx->tools, ctx->llm);
	log_info("registered text_qa tool");

	img_gen_init(&ctx->tools, ctx->img_llm, ctx->tctx);
	log_info("registered img_gen tool");

	img_edit_init(&ctx->tools, ctx->llm);
	log_info("registered img_edit tool");

	img_info_init(&ctx->tools);
	log_info("registered img_info tool");

	file_read_init(&ctx->tools);
	log_info("registered file_read tool");

	file_list_init(&ctx->tools);
	log_info("registered file_list tool");

	file_info_init(&ctx->tools);
	log_info("registered file_info tool");

	if (ctx->config.react.bash_exec_enabled) {
		for (int i = 0;
		     i < ctx->config.react.bash_exec_allowed_commands_count; i++)
			tool_context_allow_command(
				ctx->tctx,
				ctx->config.react.bash_exec_allowed_commands[i]);
		for (int i = 0;
		     i < ctx->config.react.bash_exec_allowed_cwds_count; i++)
			tool_context_allow_exec_dir(
				ctx->tctx,
				ctx->config.react.bash_exec_allowed_cwds[i]);
		tool_context_set_command_approval(
			ctx->tctx, command_approval_callback, ctx);
		bash_exec_init(&ctx->tools, ctx->tctx);
		log_info("registered bash_exec tool (explicitly enabled)");
	} else {
		log_info("bash_exec tool disabled by default");
	}

	img_resize_init(&ctx->tools, ctx->tctx);
	log_info("registered img_resize tool");

	img_convert_init(&ctx->tools, ctx->tctx);
	log_info("registered img_convert tool");

	vid_gen_init(&ctx->tools, ctx->vid_llm, ctx->tctx);
	log_info("registered vid_gen tool");

	{
		static const char *readonly_tools[] = {
			"file_read", "file_list", "file_info",
			"img_info", "text_qa", NULL
		};
		for (const char **t = readonly_tools; *t; t++) {
			struct tool_entry *e = tool_lookup(&ctx->tools, *t);
			if (e)
				e->flags |= TOOL_FLAG_READONLY;
		}
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

	plan_registry_init(&ctx->plans);
	rc = plan_tool_init(&ctx->tools, &ctx->plans, ctx->llm);
	if (rc < 0)
		log_err("failed to register plan tool: %s", morph_strerror(rc));
	else
		log_info("registered plan tool");

	ask_user_init(&ctx->tools, cli_ask_user_callback, ctx);
	ctx->react->ask_user_fn = cli_ask_user_callback;
	ctx->react->ask_user_data = ctx;
	log_info("registered ask_user tool");

	img_annotate_init(&ctx->tools, cli_img_annotate_pause,
			  cli_img_annotate_resume, ctx);
	log_info("registered img_annotate tool");

	for (int i = 0; i < ctx->config.react.disabled_tools_count; i++) {
		tool_disable(&ctx->tools, ctx->config.react.disabled_tools[i]);
	}

	ctx->react->skills = ctx->skills;

	return 0;
}

/*
 * Discover and load extensions from the ~/.morph/exts directory.
 * ctx - CLI context with tool registry initialized.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int cli_init_exts(struct cli_context *ctx)
{
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
			int rc = ext_load(&ex, ed_path);
			if (rc == 0 && ex.enabled) {
				if (ex.manifest.purpose == EXT_PURPOSE_GUARDRAIL) {
					enum guardrail_hook gh =
						GUARDRAIL_HOOK_OUTPUT;
					if (strcmp(ex.manifest.hook, "input")
					    == 0)
						gh = GUARDRAIL_HOOK_INPUT;
					else if (strcmp(ex.manifest.hook,
							"tool_output") == 0)
						gh =
						GUARDRAIL_HOOK_TOOL_OUTPUT;
					enum guardrail_ext_type et =
						GUARDRAIL_EXT_EXEC;
					if (strcmp(ex.manifest.type, "so")
					    == 0)
						et = GUARDRAIL_EXT_SO;
					guardrail_rule_register(
						&ctx->react->guardrail,
						ex.manifest.name, gh,
						GUARDRAIL_RULE_EXT, NULL,
						ex.manifest.description,
						NULL,
						ex.manifest.action_text[0]
							? ex.manifest.action_text
							: NULL);
					if (et == GUARDRAIL_EXT_SO) {
						struct guardrail_rule *r =
							guardrail_rule_lookup(
							  &ctx->react->guardrail,
							  ex.manifest.name);
						if (r) {
							r->ext_type =
								GUARDRAIL_EXT_SO;
							char full[1024];
							snprintf(full,
								 sizeof(full),
								 "%s/%s",
								 ed_path,
								 ex.manifest.entry);
							strncpy(r->ext_entry,
								full,
								sizeof(r->ext_entry)
								- 1);
							guardrail_ext_so_load(
								r);
						}
					} else {
						struct guardrail_rule *r =
							guardrail_rule_lookup(
							  &ctx->react->guardrail,
							  ex.manifest.name);
						if (r) {
							r->ext_type =
								GUARDRAIL_EXT_EXEC;
							char full[1024];
							snprintf(full,
								 sizeof(full),
								 "%s/%s",
								 ed_path,
								 ex.manifest.entry);
							strncpy(r->ext_entry,
								full,
								sizeof(r->ext_entry)
								- 1);
						}
					}
					log_info("registered guardrail ext: %s",
						 ex.manifest.name);
					ext_unload(&ex);
					continue;
				}
				struct ext *ex_ptr = malloc(sizeof(*ex_ptr));
				if (ex_ptr) {
					memcpy(ex_ptr, &ex, sizeof(ex));
					tool_register(&ctx->tools, ex.manifest.name,
						      ex.manifest.description,
						      ex.manifest.args_schema ?
						      ex.manifest.args_schema : "",
						      ext_run_wrapper, ex_ptr,
						      ext_user_data_destroy);
					log_info("registered ext: %s", ex.manifest.name);
				}
			} else {
				ext_unload(&ex);
			}
		}
		file_free_list(ext_dirs, ext_count);
	}
	return 0;
}

/*
 * Initialize MCP servers from config and auto-connect those with auto_connect=true.
 * ctx - CLI context with config and tool registry initialized.
 *
 * Returns 0 on success, negative errno on failure.
 */
static int cli_init_mcp(struct cli_context *ctx)
{
	mcp_registry_init(&ctx->mcp);
	for (int i = 0; i < ctx->config.mcp.server_count; i++) {
		struct mcp_server_config scfg;
		memset(&scfg, 0, sizeof(scfg));
		struct config_mcp_server *cs = &ctx->config.mcp.servers[i];

		strncpy(scfg.name, cs->name, MCP_NAME_MAX - 1);
		scfg.transport = (strcmp(cs->transport, "http") == 0)
			       ? MCP_TRANSPORT_STREAMABLE_HTTP
			       : MCP_TRANSPORT_STDIO;

		if (scfg.transport == MCP_TRANSPORT_STDIO) {
			strncpy(scfg.command, cs->command, sizeof(scfg.command) - 1);
			scfg.cmd_args_count = cs->args_count;
			for (int j = 0; j < cs->args_count; j++)
				strncpy(scfg.cmd_args[j], cs->args[j], MCP_CMD_ARG_MAX - 1);
			scfg.env_count = cs->env_count;
			for (int j = 0; j < cs->env_count; j++) {
				strncpy(scfg.env_keys[j], cs->env_keys[j], 63);
				strncpy(scfg.env_vals[j], cs->env_vals[j], MCP_ENV_VAL_MAX - 1);
			}
		} else {
			strncpy(scfg.http_url, cs->http_url, sizeof(scfg.http_url) - 1);
			strncpy(scfg.http_auth_token_env, cs->http_auth_token_env, 63);
		}

		scfg.auto_connect = cs->auto_connect;
		scfg.connect_timeout = cs->connect_timeout;

		int rc = mcp_registry_add(&ctx->mcp, &scfg);
		if (rc == 0) {
			log_info("mcp: registered server '%s'%s", scfg.name,
				 scfg.auto_connect ? " (auto_connect)" : "");
		}
	}

	for (int i = 0; i < ctx->mcp.count; i++) {
		struct mcp_client *mc = ctx->mcp.servers[i];
		if (!mc->config.auto_connect)
			continue;
		int timeout = mc->config.connect_timeout;
		log_info("mcp: auto-connecting '%s'%s...",
			 mc->config.name,
			 timeout > 0 ? " (timeout enabled)" : "");
		if (timeout > 0) {
			struct auto_connect_work w;
			w.client = mc;
			w.tools = &ctx->tools;
			w.result = -1;
			w.done = 0;
			pthread_mutex_init(&w.lock, NULL);
			pthread_cond_init(&w.cond, NULL);
			pthread_t tid;
			int terr = pthread_create(&tid, NULL,
						  auto_connect_thread, &w);
			if (terr != 0) {
				pthread_mutex_destroy(&w.lock);
				pthread_cond_destroy(&w.cond);
				log_warn("mcp: auto-connect thread failed for '%s'",
					 mc->config.name);
				continue;
			}
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += timeout;
			pthread_mutex_lock(&w.lock);
			while (!w.done)
				pthread_cond_timedwait(&w.cond, &w.lock, &ts);
			pthread_mutex_unlock(&w.lock);
			pthread_join(tid, NULL);
			pthread_mutex_destroy(&w.lock);
			pthread_cond_destroy(&w.cond);
			if (w.done && w.result == 0)
				log_info("mcp: auto-connected '%s'",
					 mc->config.name);
			else if (w.done)
				log_warn("mcp: auto-connect failed for '%s': %d",
					 mc->config.name, w.result);
			else
				log_warn("mcp: auto-connect timed out for '%s'",
					 mc->config.name);
		} else {
			int rc3 = mcp_ensure_connected(mc);
			if (rc3 == 0) {
				mcp_register_server_tools(mc, &ctx->tools);
				mcp_register_server_resources(mc, &ctx->tools);
				mcp_register_server_prompts(mc, &ctx->tools);
				log_info("mcp: auto-connected '%s'",
					 mc->config.name);
			} else {
				log_warn("mcp: auto-connect failed for '%s': %d",
					 mc->config.name, rc3);
			}
		}
	}

	return 0;
}

/*
 * Initialize the CLI context: load config, open database, create models,
 * register tools, discover extensions and MCP servers, and prepare session.
 * ctx - CLI context to initialize (must be zeroed by caller or here).
 * config_path - Path to config file, or NULL for default.
 * workdir - Override output directory, or NULL for config default.
 *
 * Returns 0 on success, negative errno on failure.
 */
int cli_init(struct cli_context *ctx, const char *config_path,
	     const char *workdir)
{
	if (!ctx)
		return -EINVAL;
	memset(ctx, 0, sizeof(*ctx));

	int rc;

	rc = cli_init_config(ctx, config_path);
	if (rc < 0)
		return rc;

	if (workdir && *workdir)
		strncpy(ctx->config.general.output_dir, workdir,
			sizeof(ctx->config.general.output_dir) - 1);

	rc = cli_init_database(ctx);
	if (rc < 0)
		return rc;

	rc = cli_init_models(ctx);
	if (rc < 0)
		goto fail;

	rc = cli_init_tools(ctx);
	if (rc < 0)
		goto fail;

	rc = cli_init_exts(ctx);
	if (rc < 0)
		goto fail;

	rc = cli_init_mcp(ctx);
	if (rc < 0)
		goto fail;

	rc = session_create(&ctx->database, ctx->current_session.name,
			    ctx->config.models.text.model, &ctx->current_session);
	if (rc == -EEXIST) {
		rc = session_get_by_name(&ctx->database, ctx->current_session.name,
					&ctx->current_session);
		if (rc < 0) {
			log_err("failed to get default session");
			goto fail;
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

fail:
	cli_shutdown(ctx);
	return rc;
}

/* ---- sigint ---- */

static volatile sig_atomic_t sigint_received = 0;

static void sigint_handler(int sig)
{
	(void)sig;
	react_sigint_flag = 1;
	react_cancel_active();
	sigint_received = 1;
	if (write(STDOUT_FILENO, "\n", 1) < 0) {
		/* ignore */
	}
}

#ifdef HAVE_READLINE

static struct cli_context *g_comp_ctx;

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
			return strdup(name);
	}
	return NULL;
}

static char *session_completion_generator(const char *text, int state)
{
	static struct session *slist;
	static int scount;
	static int idx;
	static int len;

	if (state == 0) {
		if (slist) {
			free(slist);
			slist = NULL;
		}
		scount = 0;
		idx = 0;
		len = (int)strlen(text);
		if (!g_comp_ctx)
			return NULL;
		session_list(&g_comp_ctx->database, &slist, &scount, 0, NULL);
	}

	while (idx < scount) {
		struct session *s = &slist[idx];
		idx++;
		if (s->display_id[0] &&
		    strncmp(s->display_id, text, (size_t)len) == 0)
			return strdup(s->display_id);
		if (s->name[0] &&
		    strncmp(s->name, text, (size_t)len) == 0)
			return strdup(s->name);
	}

	if (slist) {
		free(slist);
		slist = NULL;
	}
	scount = 0;
	return NULL;
}

static int is_session_arg_command(const char *cmd)
{
	return (strcmp(cmd, "/switch") == 0 ||
		strcmp(cmd, "/s") == 0 ||
		strcmp(cmd, "/delete") == 0 ||
		strcmp(cmd, "/del") == 0);
}

static char **cmd_completion(const char *text, int start, int end)
{
	(void)end;
	rl_attempted_completion_over = 1;
	if (start == 0)
		return rl_completion_matches(text, cmd_completion_generator);

	char *cmd = strndup(rl_line_buffer, (size_t)(start - 1));
	int match = is_session_arg_command(cmd);
	free(cmd);
	if (match)
		return rl_completion_matches(text, session_completion_generator);

	return NULL;
}

#endif

/* ---- cli_run_once ---- */

static void emit_trace_json(struct cli_context *ctx, double elapsed)
{
	if (!ctx->react)
		return;
	cJSON *root = cJSON_CreateObject();
	switch (ctx->react->state) {
	case REACT_STATE_DONE:
		cJSON_AddStringToObject(root, "state", "done");
		break;
	case REACT_STATE_ABORT:
		cJSON_AddStringToObject(root, "state", "abort");
		break;
	case REACT_STATE_TOOL_FAIL:
		cJSON_AddStringToObject(root, "state", "tool_fail");
		break;
	default:
		cJSON_AddStringToObject(root, "state", "unknown");
		break;
	}
	if (ctx->react->final_answer)
		cJSON_AddStringToObject(root, "final_answer",
					ctx->react->final_answer);
	else
		cJSON_AddStringToObject(root, "final_answer", "");
	cJSON *steps = cJSON_CreateArray();
	struct react_step *cur = ctx->react->steps;
	while (cur) {
		cJSON *s = cJSON_CreateObject();
		cJSON_AddStringToObject(s, "type",
					react_step_type_name(cur->type));
		if (cur->content)
			cJSON_AddStringToObject(s, "content", cur->content);
		if (cur->tool_name)
			cJSON_AddStringToObject(s, "tool_name", cur->tool_name);
		if (cur->tool_args)
			cJSON_AddStringToObject(s, "tool_args", cur->tool_args);
		cJSON_AddItemToArray(steps, s);
		cur = cur->next;
	}
	cJSON_AddItemToObject(root, "steps", steps);
	cJSON_AddNumberToObject(root, "elapsed_seconds", elapsed);
	char *json = cJSON_PrintUnformatted(root);
	printf("%s\n", json);
	free(json);
	cJSON_Delete(root);
}

void cli_run_once(struct cli_context *ctx, const char *prompt)
{
	if (!ctx || !prompt)
		return;
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigint_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	struct timespec ts_start, ts_end;
	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	sigint_received = 0;
	if (ctx->react)
		react_cancel(ctx->react);
	/*
	 * In trace-json mode, redirect stdout to stderr so that
	 * only the JSON trace appears on stdout for machine parsing.
	 */
	int saved_stdout = -1;
	if (ctx->trace_json) {
		saved_stdout = dup(STDOUT_FILENO);
		dup2(STDERR_FILENO, STDOUT_FILENO);
	}
	cli_handle_command(ctx, prompt);
	if (saved_stdout >= 0) {
		fflush(stdout);
		dup2(saved_stdout, STDOUT_FILENO);
		close(saved_stdout);
	}
	clock_gettime(CLOCK_MONOTONIC, &ts_end);
	double elapsed = (ts_end.tv_sec - ts_start.tv_sec)
			 + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
	if (ctx->trace_json)
		emit_trace_json(ctx, elapsed);
	signal(SIGINT, SIG_DFL);
}

/* ---- cli_run ---- */

void cli_run(struct cli_context *ctx)
{
	if (!ctx)
		return;
	printf("morph v" MORPH_VERSION "  |  " ANSI_DIM "/help 查看命令" ANSI_RESET "\n\n");
	char line[8192];

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sigint_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);

#ifdef HAVE_READLINE
	using_history();
	g_comp_ctx = ctx;
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
				size_t chunk = (size_t)(close - p) + 1;
				memcpy(out + olen, p, chunk);
				olen += chunk;
				p = close + 1;
				continue;
			}
		}
		if (p[0] == '[' && p[1] != ']') {
			const char *close = strchr(p + 1, ')');
			if (close) {
				size_t chunk = (size_t)(close - p) + 1;
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
			size_t pre = (size_t)(path_start - p);
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


/* ---- output_callback helpers ---- */

/*
 * Handle THOUGHT step: update spinner with streaming thought content.
 * ctx - CLI context.
 * content - Thought content fragment (may be NULL or empty).
 *
 * Returns 0 always.
 */
static int output_handle_thought(struct cli_context *ctx, const char *content)
{
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
	return 0;
}

/*
 * Handle ACTION step: show tool execution spinner.
 * ctx - CLI context.
 * content - Action description, typically "tool_name(args)".
 *
 * Returns 0 always.
 */
static int output_handle_action(struct cli_context *ctx, const char *content)
{
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
		return 0;
	}
	if (content && strstr(content, " completed")) {
		return 0;
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
		ctx->last_tool_was_plan = (strcmp(tool_name, "plan") == 0);
		if (!ctx->spin.running) {
			spin_start(&ctx->spin, SPIN_STATE_EXECUTING,
				   tool_name[0] ? tool_name : "Executing");
		} else {
			spin_update(&ctx->spin, tool_name[0] ? tool_name : "Executing");
		}
		if (content)
			spin_set_sub(&ctx->spin, content);
	}
	return 0;
}

/*
 * Handle OBSERVATION step: stop spinner with appropriate status.
 * ctx - CLI context.
 * content - Observation content (tool result or error).
 *
 * Returns 0 always.
 */
static int output_handle_observation(struct cli_context *ctx, const char *content)
{
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
	if (ctx->last_tool_was_plan && content && *content
	    && strncmp(content, "image generated:", 15) != 0
	    && strncmp(content, "video generated:", 16) != 0) {
		printf("\n");
		size_t len = strlen(content);
		if (len > 2000) {
			printf("%.1997s...\n", content);
		} else {
			printf("%s\n", content);
		}
		fflush(stdout);
	}
	return 0;
}

/*
 * Handle REFLECTION step: display guardrail message.
 * ctx - CLI context.
 * content - Reflection message from guardrail.
 *
 * Returns 0 always.
 */
static int output_handle_reflection(struct cli_context *ctx, const char *content)
{
	if (ctx->streaming) {
		spin_set_sub(&ctx->spin, NULL);
		ctx->streaming = 0;
	}
	spin_pause(&ctx->spin);
	printf("\r\033[K");
	printf(ANSI_BOLD ANSI_CYAN "🛡 Guardrail" ANSI_RESET " %s\n",
	       content ? content : "");
	fflush(stdout);
	spin_resume(&ctx->spin);
	return 0;
}

/*
 * Handle FINAL step: render the final answer as markdown.
 * ctx - CLI context.
 * content - Final answer content (may be NULL or empty).
 *
 * Returns 0 always.
 */
static int output_handle_final(struct cli_context *ctx, const char *content)
{
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
	return 0;
}

/*
 * ReAct output callback: dispatch step-type events to per-type handlers.
 * type - Step type (THOUGHT, ACTION, OBSERVATION, REFLECTION, FINAL).
 * content - Step content string.
 * user_data - Pointer to cli_context.
 *
 * Returns 0 always.
 */
static int output_callback(enum react_step_type type, const char *content,
			   void *user_data)
{
	struct cli_context *ctx = user_data;

	switch (type) {
	case REACT_STEP_THOUGHT:
		return output_handle_thought(ctx, content);
	case REACT_STEP_ACTION:
		return output_handle_action(ctx, content);
	case REACT_STEP_OBSERVATION:
		return output_handle_observation(ctx, content);
	case REACT_STEP_REFLECTION:
		return output_handle_reflection(ctx, content);
	case REACT_STEP_FINAL:
		return output_handle_final(ctx, content);
	}
	return 0;
}


static int cli_ask_user_callback(const char *question,
				  const char *const *choices,
				  int choices_count,
				  char **answer,
				  void *user_data)
{
	struct cli_context *ctx = user_data;
	if (!ctx || !answer)
		return -EINVAL;

	spin_pause(&ctx->spin);

	printf("\r\033[K");
	printf(ANSI_BOLD ANSI_CYAN "? %s" ANSI_RESET "\n", question);

	char prompt[64];
	if (choices && choices_count > 0) {
		for (int i = 0; i < choices_count; i++)
			printf("  %d. %s\n", i + 1, choices[i]);
		snprintf(prompt, sizeof(prompt),
			 "  [" ANSI_GREEN "1-%d" ANSI_RESET "]: ",
			 choices_count);
	} else {
		snprintf(prompt, sizeof(prompt), "  > ");
	}
#ifdef HAVE_READLINE
	{
		char *input = NULL;
		FILE *tty = fopen("/dev/tty", "r");
		if (tty) {
			FILE *old_in = rl_instream;
			rl_instream = tty;
			input = readline(prompt);
			rl_instream = old_in;
			fclose(tty);
		} else {
			input = readline(prompt);
		}
		if (!input) {
			printf("\n");
			spin_resume(&ctx->spin);
			*answer = strdup("");
			return -EIO;
		}
		if (input[0] != '\0')
			add_history(input);
		if (choices && choices_count > 0) {
			int n = atoi(input);
			if (n >= 1 && n <= choices_count) {
				*answer = strdup(choices[n - 1]);
				free(input);
			} else {
				*answer = input;
			}
		} else {
			*answer = input;
		}
	}
#else
	fflush(stdout);
	char buf[1024] = {0};
	FILE *tty = fopen("/dev/tty", "r");
	if (!tty) {
		printf("\n");
		spin_resume(&ctx->spin);
		*answer = strdup("");
		return -ENOTTY;
	}
	if (!fgets(buf, sizeof(buf), tty)) {
		fclose(tty);
		printf("\n");
		spin_resume(&ctx->spin);
		*answer = strdup("");
		return -EIO;
	}
	fclose(tty);

	buf[strcspn(buf, "\n")] = '\0';

	if (choices && choices_count > 0) {
		int n = atoi(buf);
		if (n >= 1 && n <= choices_count)
			*answer = strdup(choices[n - 1]);
		else
			*answer = strdup(buf);
	} else {
		*answer = strdup(buf);
	}
#endif

	spin_resume(&ctx->spin);
	return 0;
}

/*
 * Generic y/n/a prompt shared by HITL and bash_exec approval flows.
 *
 * subject - Short label rendered after the "Approved (...)"/"Denied (...)"
 *           summary so the user knows which decision they made.
 *
 * Returns:
 *   0 - denied
 *   1 - approved once
 *   2 - approved with "always" semantics
 */
static int prompt_yna(const char *subject)
{
#ifdef HAVE_READLINE
	char *rl_input = readline("  [y]es / [n]o / [a]lways: ");
	if (!rl_input) {
		printf("\n");
		return 0;
	}
	int v;
	if (rl_input[0] == 'a' || rl_input[0] == 'A')
		v = 2;
	else if (rl_input[0] == 'y' || rl_input[0] == 'Y')
		v = 1;
	else
		v = 0;
	free(rl_input);
#else
	printf("  [" ANSI_GREEN "y" ANSI_RESET "]es / ["
	       ANSI_RED "n" ANSI_RESET "]o / [a]lways: ");
	fflush(stdout);

	char buf[16];
	FILE *tty = fopen("/dev/tty", "r");
	if (!tty) {
		printf("\n");
		return 0;
	}
	if (!fgets(buf, sizeof(buf), tty)) {
		fclose(tty);
		printf("\n");
		return 0;
	}
	fclose(tty);

	int v;
	if (buf[0] == 'a' || buf[0] == 'A')
		v = 2;
	else if (buf[0] == 'y' || buf[0] == 'Y')
		v = 1;
	else
		v = 0;
#endif

	if (v == 1 || v == 2)
		printf(ANSI_BOLD ANSI_GREEN "  ✓ Approved" ANSI_RESET " (%s%s)\n",
		       subject ? subject : "",
		       v == 2 ? ", always" : "");
	else
		printf(ANSI_BOLD ANSI_RED "  ✗ Denied" ANSI_RESET " (%s)\n",
		       subject ? subject : "");

	fflush(stdout);
	return v;
}

static enum hitl_verdict hitl_approval_callback(const char *tool_name,
						const char *tool_args,
						void *user_data)
{
	struct cli_context *ctx = user_data;
	if (!ctx)
		return HITL_DENY;

	spin_pause(&ctx->spin);

	printf("\r\033[K");
	printf(ANSI_BOLD ANSI_YELLOW "⚠ Approval Required" ANSI_RESET "\n");
	printf("  Tool: " ANSI_BOLD "%s" ANSI_RESET "\n", tool_name);

	if (tool_args && *tool_args && strcmp(tool_args, "{}") != 0) {
		char display_args[512];
		strncpy(display_args, tool_args, sizeof(display_args) - 1);
		display_args[sizeof(display_args) - 1] = '\0';
		size_t alen = strlen(display_args);
		if (alen > 200) {
			display_args[197] = '.';
			display_args[198] = '.';
			display_args[199] = '.';
			display_args[200] = '\0';
		}
		printf("  Args: " ANSI_DIM "%s" ANSI_RESET "\n", display_args);
	}

	int v = prompt_yna(tool_name);
	if (v == 2)
		return HITL_ALWAYS;
	if (v == 1)
		return HITL_APPROVE;
	return HITL_DENY;
}

static enum command_verdict command_approval_callback(const char *command,
						      const char *cwd,
						      void *user_data)
{
	struct cli_context *ctx = user_data;
	if (!ctx)
		return COMMAND_DENY;

	spin_pause(&ctx->spin);

	printf("\r\033[K");
	printf(ANSI_BOLD ANSI_YELLOW "⚠ Shell Command Approval" ANSI_RESET "\n");

	if (command) {
		char display[512];
		strncpy(display, command, sizeof(display) - 1);
		display[sizeof(display) - 1] = '\0';
		size_t alen = strlen(display);
		if (alen > 380) {
			display[377] = '.';
			display[378] = '.';
			display[379] = '.';
			display[380] = '\0';
		}
		printf("  Cmd:  " ANSI_BOLD "%s" ANSI_RESET "\n", display);
	}
	if (cwd && *cwd)
		printf("  Cwd:  " ANSI_DIM "%s" ANSI_RESET "\n", cwd);
	printf("  " ANSI_DIM "'always' will trust this program (and cwd) "
	       "for the rest of the session." ANSI_RESET "\n");

	int v = prompt_yna("bash_exec");
	if (v == 2)
		return COMMAND_ALWAYS;
	if (v == 1)
		return COMMAND_ALLOW;
	return COMMAND_DENY;
}

static enum write_verdict write_approval_callback(const char *path,
						   const char *output_dir,
						   void *user_data)
{
	struct cli_context *ctx = user_data;
	if (!ctx)
		return WRITE_DENY;

	spin_pause(&ctx->spin);

	printf("\r\033[K");
	printf(ANSI_BOLD ANSI_YELLOW "⚠ Write Path Approval" ANSI_RESET "\n");
	printf("  Path:  " ANSI_BOLD "%s" ANSI_RESET "\n", path);
	printf("  " ANSI_DIM "Output dir: %s" ANSI_RESET "\n", output_dir);
	printf("  " ANSI_DIM "'always' will trust this directory "
	       "for the rest of the session." ANSI_RESET "\n");

	int v = prompt_yna("write_path");
	if (v == 2)
		return WRITE_ALWAYS;
	if (v == 1)
		return WRITE_ALLOW;
	return WRITE_DENY;
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

	printf(ANSI_BOLD ANSI_CYAN "▸ %s" ANSI_RESET "\n", input);
	fflush(stdout);

	sigint_received = 0;
	if (ctx->react)
		react_cancel(ctx->react);

	cli_refresh_memory_context(ctx, effective_input);

	react_run(ctx->react, effective_input, output_callback, ctx);

	if (ctx->spin.running) {
		if (ctx->react && ctx->react->state == REACT_STATE_ABORT &&
		    ctx->react->cancelled) {
			spin_stop(&ctx->spin, SPIN_STATE_ABORT, "Cancelled");
		} else {
			spin_stop(&ctx->spin, SPIN_STATE_ERROR, "Error");
		}
		printf("\n");
	}

	if (ctx->react && ctx->react->state == REACT_STATE_ABORT) {
		printf(ANSI_YELLOW "[aborted] ReAct loop cancelled or timed out." ANSI_RESET "\n");
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
	if (ctx->react) {
		struct memory_options mem_opts = cli_memory_options(ctx);
		/* Run consolidation on a background worker so the prompt
		 * returns immediately. The LLM extraction path is the
		 * slow one (1-3s blocking HTTP); offloading it keeps the
		 * REPL responsive. */
		int async_rc = memory_consolidate_turn_async(
			&ctx->database, ctx->current_session.id,
			effective_input, ctx->react->final_answer,
			ctx->react->steps,
			ctx->react->state == REACT_STATE_DONE,
			&mem_opts);
		if (async_rc != 0) {
			memory_consolidate_turn(&ctx->database,
						ctx->current_session.id,
						effective_input,
						ctx->react->final_answer,
						ctx->react->steps,
						ctx->react->state == REACT_STATE_DONE,
						&mem_opts);
		}
	}
	ctx->streaming = 0;
	return 0;
}

/* ---- cli_shutdown ---- */

void cli_shutdown(struct cli_context *ctx)
{
	if (!ctx)
		return;
	/* Drain the memory async worker before tearing down the db so
	 * any in-flight consolidation job finishes against a live file. */
	memory_async_shutdown();
	if (ctx->react)
		react_context_destroy(ctx->react);
	if (ctx->tokenizer)
		tokenizer_destroy(ctx->tokenizer);
	tool_registry_cleanup(&ctx->tools);
	if (ctx->tctx) {
		tool_context_destroy(ctx->tctx);
		ctx->tctx = NULL;
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
	mcp_registry_cleanup(&ctx->mcp);
	db_close(&ctx->database);
	log_info("cli shutdown complete");
}
