#include "cli.h"
#include "util/log.h"
#include "util/file.h"
#include "agent/tokenizer.h"
#include "agent/tools/text_gen.h"
#include "skill/skill.h"
#include "agent/tools/text_qa.h"
#include "agent/tools/img_gen.h"
#include "agent/tools/img_edit.h"
#include "agent/tools/img_info.h"
#include "agent/tools/vid_gen.h"
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

static const char *default_db_path = "~/.multi-agent/data.db";
static const char *default_config_path = "~/.multi-agent/config.toml";

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
static int cmd_skill(struct cli_context *ctx, int argc, char **argv);
static int cmd_export_alias(struct cli_context *ctx, int argc, char **argv);

/* ---- dispatch table ---- */

struct cmd_entry {
	const char *name;
	int (*handler)(struct cli_context *ctx, int argc, char **argv);
	const char *desc;
	const char *usage;
};

static const struct cmd_entry commands[] = {
	{ "/quit",    cmd_quit,    "Exit the program",                  "" },
	{ "/q",       cmd_quit,    "Alias for /quit",                   "" },
	{ "/help",    cmd_help,    "Show help for commands",            "/help [command]" },
	{ "/new",     cmd_new,     "Create a new session",              "/new [name]" },
	{ "/switch",  cmd_switch,  "Switch to another session",         "/switch <name|id>" },
	{ "/list",    cmd_list,    "List all sessions",                 "/list" },
	{ "/rename",  cmd_rename,  "Rename current session",            "/rename <new_name>" },
	{ "/delete",  cmd_delete,  "Delete a session",                  "/delete <name|id>" },
	{ "/history", cmd_history, "Show recent messages",              "/history [n|--all]" },
	{ "/model",   cmd_model,   "View or switch the LLM model",      "/model [name]" },
	{ "/trace",   cmd_trace,   "Show ReAct trace for current turn", "/trace [--from-db]" },
	{ "/context", cmd_context, "Show token usage and context info", "/context" },
	{ "/compress",cmd_compress,"Manually compress context window",  "/compress" },
	{ "/save",    cmd_save,    "Export session to a file",          "/save [format]" },
	{ "/config",  cmd_config,  "View current configuration",        "/config" },
	{ "/image",   cmd_image,   "Inject an image into context",      "/image <file_path>" },
	{ "/video",   cmd_video,   "Inject a video (M3)",               "/video <file_path>" },
	{ "/skill",   cmd_skill,   "List or manage tools and skills",   "/skill list" },
	{ "/export",  cmd_export_alias, "Alias for /save",              "/export <format>" },
};

static const int num_commands = (int)(sizeof(commands) / sizeof(commands[0]));

/* ---- skill_run wrapper ---- */

static int skill_run_wrapper(const char *args_json, char **result_json, void *user_data)
{
	struct skill *sk = user_data;
	if (!sk)
		return -EINVAL;
	return skill_run(sk, args_json, result_json);
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
	printf(ANSI_BOLD "multi-agent commands:" ANSI_RESET "\n");
	for (int i = 0; i < num_commands; i++) {
		if (strcmp(commands[i].name, "/q") == 0)
			continue;
		printf("  %-20s %s\n", commands[i].name, commands[i].desc);
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
		CMD_OK("created and switched to session: %s", name);
	} else {
		CMD_ERROR("failed to create session: %s", name);
	}
	return rc;
}

static int cmd_switch(struct cli_context *ctx, int argc, char **argv)
{
	const char *name = cmd_arg(argc, argv, 1);
	if (!name) {
		CMD_ERROR("usage: /switch <name|id>");
		return -EINVAL;
	}
	struct session s;
	int rc = session_get_by_name(&ctx->database, name, &s);
	if (rc < 0) {
		char *end;
		long id = strtol(name, &end, 10);
		if (*end == '\0')
			rc = session_get_by_id(&ctx->database, (int64_t)id, &s);
	}
	if (rc == 0) {
		ctx->current_session = s;
		CMD_OK("switched to session: %s", s.name);
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
	printf("  %-5s %-30s %-35s %s\n", "ID", "Name", "Model", "Tokens");
	printf("  %-5s %-30s %-35s %s\n", "---", "---", "---", "---");
	for (int i = 0; i < count; i++) {
		int is_current = (list[i].id == ctx->current_session.id);
		printf("  %s%-5lld%s %-30s %-35s %lld\n",
		       is_current ? ANSI_GREEN : "",
		       (long long)list[i].id,
		       is_current ? ANSI_RESET : "",
		       list[i].name,
		       list[i].model,
		       (long long)list[i].token_used);
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
		printf(ANSI_DIM "[%s]" ANSI_RESET " %s\n", label,
		       cur->content ? cur->content : "(empty)");
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

static struct react_step *json_to_react_steps(const char *json, int *out_count)
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
		else if (strcmp(type_name, "Final") == 0)	type = REACT_STEP_FINAL;
		cJSON *content = cJSON_GetObjectItem(obj, "content");
		cJSON *tool_name = cJSON_GetObjectItem(obj, "tool_name");
		cJSON *tool_args = cJSON_GetObjectItem(obj, "tool_args");
		char args_buf[1024] = {0};
		if (cJSON_IsString(tool_args) && tool_args->valuestring)
			snprintf(args_buf, sizeof(args_buf), "%s(%s)",
				 cJSON_IsString(tool_name) ? tool_name->valuestring : "",
				 tool_args->valuestring);
		struct react_step *s = react_step_create(
			type,
			cJSON_IsString(content) ? content->valuestring : NULL,
			cJSON_IsString(tool_name) ? tool_name->valuestring : NULL,
			args_buf[0] ? args_buf : NULL);
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

static void free_json_react_steps(struct react_step *steps)
{
	while (steps) {
		struct react_step *next = steps->next;
		free(steps->content);
		free(steps->tool_name);
		free(steps->tool_args);
		free(steps);
		steps = next;
	}
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
		struct react_step *steps = json_to_react_steps(json, &count);
		if (steps) {
			print_trace_steps(steps, count, aborted ? "ABORT" : "DONE");
			free_json_react_steps(steps);
		} else {
			printf("  (raw) %s\n", json);
		}
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
	int remove_count = count - keep;
	struct message *cur = msgs;
	int removed = 0;
	while (cur && removed < remove_count) {
		int rc = message_delete(&ctx->database, cur->id);
		if (rc == 0)
			removed++;
		struct message *next = cur->next;
		free(cur->content);
		free(cur);
		cur = next;
	}
	if (cur)
		message_free_list(cur);
	CMD_OK("compressed: removed %d old messages, kept %d recent", removed, keep);
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
	if (video_play(argv[1]) != 0) {
		CMD_ERROR("failed to play video: %s", argv[1]);
		return -EIO;
	}
	CMD_OK("video loaded: %s", argv[1]);
	return 0;
}

static int cmd_skill(struct cli_context *ctx, int argc, char **argv)
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
			CMD_ERROR("usage: /skill info <name>");
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
		CMD_ERROR("skill install not yet implemented (M4)");
		return 0;
	}
	if (sub && strcmp(sub, "remove") == 0) {
		CMD_ERROR("skill remove not yet implemented (M4)");
		return 0;
	}
	if (sub && strcmp(sub, "enable") == 0) {
		CMD_ERROR("skill enable not yet implemented (M4)");
		return 0;
	}
	if (sub && strcmp(sub, "disable") == 0) {
		CMD_ERROR("skill disable not yet implemented (M4)");
		return 0;
	}
	/* /skill alone → show tools */
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

static int cmd_export_alias(struct cli_context *ctx, int argc, char **argv)
{
	printf("use /save [format] instead\n");
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
	char *db_dir = file_expand_path("~/.multi-agent");
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
		return -ENOMEM;
	}
	struct compress_config compress_cfg = {
		.max_context_tokens = ctx->config.models.text.context_limit,
		.max_history_rounds = ctx->config.context.keep_recent_rounds,
		.summarize_threshold_ratio = ctx->config.context.summarize_threshold_ratio,
		.compress_target_ratio = ctx->config.context.compress_target_ratio,
	};
	ctx->react = react_context_create(&ctx->tools, ctx->tokenizer, &compress_cfg);
	if (!ctx->react) {
		log_err("failed to create react context");
		tokenizer_destroy(ctx->tokenizer);
		return -ENOMEM;
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

	/* Auto-discover skills from skills/ directory */
	char skills_dir[512] = {0};
	char *skills_home = file_expand_path("~/.multi-agent/skills");
	if (skills_home) {
		strncpy(skills_dir, skills_home, sizeof(skills_dir) - 1);
		free(skills_home);
	} else {
		strncpy(skills_dir, "skills", sizeof(skills_dir) - 1);
	}
	if (!file_exists(skills_dir))
		file_ensure_dir(skills_dir);
	char **skill_dirs = NULL;
	int skill_count = 0;
	if (file_list_dirs(skills_dir, &skill_dirs, &skill_count) == 0) {
		for (int i = 0; i < skill_count; i++) {
			char sd_path[1024];
			snprintf(sd_path, sizeof(sd_path), "%s/%s", skills_dir, skill_dirs[i]);
			struct skill sk;
			int rc2 = skill_load(&sk, sd_path);
			if (rc2 == 0 && sk.enabled) {
				struct skill *sk_ptr = malloc(sizeof(*sk_ptr));
				if (sk_ptr) {
					memcpy(sk_ptr, &sk, sizeof(sk));
					tool_register(&ctx->tools, sk.manifest.name,
						      sk.manifest.description,
						      sk.manifest.args_schema ?
						      sk.manifest.args_schema : "",
						      skill_run_wrapper, sk_ptr);
					log_info("registered skill: %s", sk.manifest.name);
				}
			} else {
				skill_unload(&sk);
			}
		}
		file_free_list(skill_dirs, skill_count);
	}

	rc = session_create(&ctx->database, ctx->current_session.name,
			    ctx->config.models.text.model, &ctx->current_session);
	if (rc == -EEXIST) {
		rc = session_get_by_name(&ctx->database, ctx->current_session.name,
					&ctx->current_session);
		if (rc < 0) {
			log_err("failed to get default session");
			return rc;
		}
	} else if (rc < 0) {
		log_err("failed to create default session");
		return rc;
	}
	ctx->running = 1;
	ctx->streaming = 0;
	ctx->image_path[0] = '\0';
	log_info("cli initialized");
	return 0;
}

/* ---- sigint ---- */

static volatile sig_atomic_t sigint_received = 0;

static void sigint_handler(int sig)
{
	(void)sig;
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
	printf("multi-agent v0.1  |  " ANSI_DIM "/help 查看命令" ANSI_RESET "\n\n");
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
		snprintf(prompt, sizeof(prompt), ANSI_GREEN "[%s]" ANSI_RESET " > ",
			 ctx->current_session.name);
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
		printf(ANSI_GREEN "[%s]" ANSI_RESET " > ", ctx->current_session.name);
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

static int output_callback(enum react_step_type type, const char *content,
			   void *user_data)
{
	struct cli_context *ctx = user_data;
	switch (type) {
	case REACT_STEP_THOUGHT:
		if (content && *content) {
			if (!ctx->streaming) {
				printf("\r\033[K" ANSI_DIM);
				ctx->streaming = 1;
			}
			fputs(content, stdout);
			fflush(stdout);
		} else if (!ctx->streaming) {
			printf(ANSI_DIM "..." ANSI_RESET);
			fflush(stdout);
			ctx->streaming = 1;
		}
		break;
	case REACT_STEP_ACTION:
		if (ctx->streaming) {
			printf(ANSI_RESET "\n");
			ctx->streaming = 0;
		}
		printf(ANSI_BOLD ANSI_YELLOW "[Action]" ANSI_RESET " %s\n",
		       content ? content : "");
		fflush(stdout);
		break;
	case REACT_STEP_OBSERVATION:
		if (ctx->streaming) {
			printf(ANSI_RESET "\n");
			ctx->streaming = 0;
		}
		printf(ANSI_DIM "[Observation]" ANSI_RESET " %s\n",
		       content ? content : "");
		if (content && strncmp(content, "image generated: ", 17) == 0) {
			const char *path_start = content + 17;
			const char *path_end = strstr(path_start, " (");
			if (!path_end)
				path_end = path_start + strlen(path_start);
			size_t plen = (size_t)(path_end - path_start);
			char *img_path = malloc(plen + 1);
			if (img_path) {
				memcpy(img_path, path_start, plen);
				img_path[plen] = '\0';
				log_info("rendering image: path='%s' exists=%d",
					 img_path, file_exists(img_path));
				image_render_terminal(img_path);
				free(img_path);
			}
		}
		if (content && strncmp(content, "video generated: ", 17) == 0) {
			const char *path_start = content + 17;
			const char *path_end = strstr(path_start, " (");
			if (!path_end)
				path_end = path_start + strlen(path_start);
			size_t plen = (size_t)(path_end - path_start);
			char *vid_path = malloc(plen + 1);
			if (vid_path) {
				memcpy(vid_path, path_start, plen);
				vid_path[plen] = '\0';
				video_play(vid_path);
				free(vid_path);
			}
		}
		fflush(stdout);
		break;
	case REACT_STEP_FINAL:
		if (ctx->streaming) {
			printf(ANSI_RESET "\n");
			ctx->streaming = 0;
		}
		if (content && *content)
			markdown_render_ansi(content);
		else
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
	int msg_count = message_count(&ctx->database, ctx->current_session.id);
	if (msg_count == 0 && input[0] != '/') {
		char title[48];
		size_t i = 0;
		while (input[i] && i < sizeof(title) - 1) {
			title[i] = input[i];
			i++;
		}
		title[i] = '\0';
		session_rename(&ctx->database, ctx->current_session.id, title);
		strncpy(ctx->current_session.name, title,
			sizeof(ctx->current_session.name) - 1);
	}

	react_run(ctx->react, effective_input, output_callback, ctx);

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
	if (ctx->llm)
		model_destroy(ctx->llm);
	if (ctx->img_llm)
		model_destroy(ctx->img_llm);
	if (ctx->vid_llm)
		model_destroy(ctx->vid_llm);
	db_close(&ctx->database);
	log_info("cli shutdown complete");
}
