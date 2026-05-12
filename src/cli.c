#include "cli.h"
#include "util/log.h"
#include "util/file.h"
#include "agent/tokenizer.h"
#include "agent/tools/text_gen.h"
#include "agent/tools/img_gen.h"
#include "agent/tools/img_edit.h"
#include "agent/tools/img_info.h"
#include "db/database.h"
#include "config.h"
#include "render/markdown.h"
#include "render/image.h"
#include "stb_image.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *default_db_path = "~/.multi-agent/data.db";
static const char *default_config_path = "~/.multi-agent/config.toml";

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

void cli_run(struct cli_context *ctx)
{
	if (!ctx)
		return;
	printf("multi-agent v0.1  |  /help 查看命令\n\n");
	char line[4096];
	while (ctx->running) {
		printf("[%s] > ", ctx->current_session.name);
		fflush(stdout);
		if (!fgets(line, sizeof(line), stdin))
			break;
		line[strcspn(line, "\n")] = '\0';
		if (line[0] == '\0')
			continue;
		cli_handle_command(ctx, line);
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
				printf("\033[2m");
				ctx->streaming = 1;
			}
			fputs(content, stdout);
			fflush(stdout);
		} else if (!ctx->streaming) {
			ctx->streaming = 1;
		}
		break;
	case REACT_STEP_ACTION:
		if (ctx->streaming) {
			printf("\033[0m\n");
			ctx->streaming = 0;
		}
		printf("\033[1m\033[33m[Action]\033[0m %s\n", content ? content : "");
		fflush(stdout);
		break;
	case REACT_STEP_OBSERVATION:
		if (ctx->streaming) {
			printf("\033[0m\n");
			ctx->streaming = 0;
		}
		printf("\033[2m[Observation]\033[0m %s\n", content ? content : "");
		if (content && strncmp(content, "image generated: ", 17) == 0) {
			const char *path_start = content + 17;
			const char *path_end = strchr(path_start, ' ');
			if (!path_end)
				path_end = path_start + strlen(path_start);
			size_t plen = (size_t)(path_end - path_start);
			char *img_path = malloc(plen + 1);
			if (img_path) {
				memcpy(img_path, path_start, plen);
				img_path[plen] = '\0';
				image_render_terminal(img_path);
				free(img_path);
			}
		}
		fflush(stdout);
		break;
	case REACT_STEP_FINAL:
		if (ctx->streaming) {
			printf("\033[0m\n");
			ctx->streaming = 0;
		}
		if (content && *content) {
			markdown_render_ansi(content);
		}
		fflush(stdout);
		break;
	}
	return 0;
}

int cli_handle_command(struct cli_context *ctx, const char *input)
{
	if (!ctx || !input)
		return -EINVAL;
	if (strcmp(input, "/quit") == 0 || strcmp(input, "/q") == 0) {
		ctx->running = 0;
		printf("Goodbye!\n");
		return 0;
	}
	if (strcmp(input, "/help") == 0 || strncmp(input, "/help ", 6) == 0) {
		cli_print_help();
		return 0;
	}
	if (strncmp(input, "/new", 4) == 0) {
		const char *name = input[4] == ' ' ? input + 5 : NULL;
		if (!name || *name == '\0')
			name = "new_session";
		struct session s;
		int rc = session_create(&ctx->database, name,
					ctx->config.models.text.model, &s);
		if (rc == 0) {
			ctx->current_session = s;
			printf("created and switched to session: %s\n", name);
		} else {
			printf("failed to create session: %s (error: %d)\n", name, rc);
		}
		return rc;
	}
	if (strncmp(input, "/switch", 7) == 0) {
		const char *name = input[7] == ' ' ? input + 8 : NULL;
		if (!name || *name == '\0') {
			printf("usage: /switch <name|id>\n");
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
			printf("switched to session: %s\n", s.name);
		} else {
			printf("session not found: %s\n", name);
		}
		return rc;
	}
	if (strcmp(input, "/list") == 0) {
		struct session *list;
		int count = 0;
		session_list(&ctx->database, &list, &count);
		printf("  %-5s %-20s %-15s %s\n", "ID", "Name", "Model", "Tokens");
		printf("  %-5s %-20s %-15s %s\n", "---", "---", "---", "---");
		for (int i = 0; i < count; i++)
			printf("  %-5lld %-20s %-15s %lld\n",
			       (long long)list[i].id, list[i].name,
			       list[i].model, (long long)list[i].token_used);
		free(list);
		return 0;
	}
	if (strncmp(input, "/rename", 7) == 0) {
		const char *new_name = input[7] == ' ' ? input + 8 : NULL;
		if (!new_name || *new_name == '\0') {
			printf("usage: /rename <new_name>\n");
			return -EINVAL;
		}
		int rc = session_rename(&ctx->database, ctx->current_session.id, new_name);
		if (rc == 0) {
			strncpy(ctx->current_session.name, new_name,
				sizeof(ctx->current_session.name) - 1);
			printf("session renamed to: %s\n", new_name);
		} else {
			printf("failed to rename session\n");
		}
		return rc;
	}
	if (strncmp(input, "/delete", 7) == 0) {
		const char *name = input[7] == ' ' ? input + 8 : NULL;
		if (!name || *name == '\0') {
			printf("usage: /delete <name|id>\n");
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
			printf("session not found: %s\n", name);
			return -ENOENT;
		}
		if (id == ctx->current_session.id) {
			printf("cannot delete current session, switch first\n");
			return -EINVAL;
		}
		int rc = session_delete(&ctx->database, id);
		if (rc == 0)
			printf("session deleted\n");
		else
			printf("failed to delete session\n");
		return rc;
	}
	if (strncmp(input, "/history", 8) == 0) {
		int n = 20;
		if (input[8] == ' ')
			n = atoi(input + 9);
		if (n <= 0)
			n = 20;
		int count = 0;
		struct message *msgs = message_list(&ctx->database,
						     ctx->current_session.id, &count);
		printf("--- session %s (showing last %d of %d) ---\n",
		       ctx->current_session.name, n > count ? count : n, count);
		struct message *cur = msgs;
		int shown = 0;
		while (cur && shown < n) {
			printf("[%s] %s\n", cur->role,
			       cur->content ? cur->content : "(empty)");
			cur = cur->next;
			shown++;
		}
		message_free_list(msgs);
		return 0;
	}
	if (strncmp(input, "/model", 6) == 0) {
		if (input[6] == ' ' && input[7]) {
			strncpy(ctx->current_session.model, input + 7,
				sizeof(ctx->current_session.model) - 1);
			session_update_model(&ctx->database, ctx->current_session.id,
					    input + 7);
			strncpy(ctx->llm->model_id, input + 7,
				sizeof(ctx->llm->model_id) - 1);
			printf("model switched to: %s\n", input + 7);
		} else {
			printf("current model: %s\n", ctx->current_session.model);
		}
		return 0;
	}
	if (strcmp(input, "/trace") == 0) {
		if (!ctx->react || !ctx->react->steps) {
			printf("no ReAct trace for current session\n");
			return 0;
		}
		printf("--- ReAct trace (session: %s) ---\n", ctx->current_session.name);
		struct react_step *cur = ctx->react->steps;
		int step = 1;
		while (cur) {
			printf("  %d. [%s]", step++, react_step_type_name(cur->type));
			if (cur->content)
				printf(" %s", cur->content);
			if (cur->tool_name)
				printf(" (tool: %s)", cur->tool_name);
			printf("\n");
			cur = cur->next;
		}
		printf("--- state: %s, steps: %d ---\n",
		       react_state_name(ctx->react->state), ctx->react->step_count);
		return 0;
	}
	if (strcmp(input, "/context") == 0) {
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
		printf("context: %d / %d tokens (%.1f%%) | messages: %d%s\n",
		       total_tokens, limit, pct, msg_count,
		       limit > 0 && pct >= 80.0 ? " | WARNING: near limit" : "");
		return 0;
	}
	if (strcmp(input, "/compress") == 0) {
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
			printf("only %d messages, no compression needed (keep %d)\n",
			       count, keep);
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
		printf("compressed: removed %d old messages, kept %d recent\n",
		       removed, keep);
		return 0;
	}
	if (strncmp(input, "/save", 5) == 0) {
		const char *fmt = "md";
		if (input[5] == ' ')
			fmt = input + 6;
		int count = 0;
		struct message *msgs = message_list(&ctx->database,
						     ctx->current_session.id, &count);
		char filename[512];
		snprintf(filename, sizeof(filename), "%s_%lld.%s",
			 ctx->current_session.name,
			 (long long)time(NULL), fmt);
		printf("saving session to: %s\n", filename);
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
			printf("saved %d messages\n", count);
		} else {
			printf("failed to open file for writing\n");
		}
		message_free_list(msgs);
		return 0;
	}
	if (strcmp(input, "/config") == 0) {
		printf("[general]\n");
		printf("  default_session = %s\n", ctx->config.general.default_session);
		printf("  output_dir = %s\n", ctx->config.general.output_dir);
		printf("  log_level = %s\n", ctx->config.general.log_level);
		printf("[model.text]\n");
		printf("  provider = %s\n", ctx->config.models.text.provider);
		printf("  model = %s\n", ctx->config.models.text.model);
		printf("  api_base = %s\n", ctx->config.models.text.api_base);
		printf("  context_limit = %d\n", ctx->config.models.text.context_limit);
		printf("[react]\n");
		printf("  max_iterations = %d\n", ctx->config.react.max_iterations);
		printf("  step_timeout = %d\n", ctx->config.react.step_timeout_seconds);
		printf("  tool_max_retries = %d\n", ctx->config.react.tool_max_retries);
		printf("[context]\n");
		printf("  threshold = %.1f\n", ctx->config.context.summarize_threshold_ratio);
		printf("  target = %.1f\n", ctx->config.context.compress_target_ratio);
		printf("  keep_rounds = %d\n", ctx->config.context.keep_recent_rounds);
		return 0;
	}
	if (strncmp(input, "/skill list", 11) == 0 || strcmp(input, "/skill") == 0) {
		printf("registered tools:\n");
		for (int i = 0; i < ctx->tools.count; i++) {
			printf("  %-15s %s\n",
			       ctx->tools.entries[i].desc.name,
			       ctx->tools.entries[i].desc.desc);
		}
		if (ctx->tools.count == 0)
			printf("  (none)\n");
		return 0;
	}
	if (strncmp(input, "/image", 6) == 0) {
		const char *path = input[6] == ' ' ? input + 7 : NULL;
		if (!path || !*path) {
			printf("usage: /image <file_path>\n");
			return -EINVAL;
		}
		char *expanded = file_expand_path(path);
		if (!file_exists(expanded)) {
			printf("file not found: %s\n", expanded);
			free(expanded);
			return -ENOENT;
		}
		int w = 0, h = 0, ch = 0;
		if (!stbi_info(expanded, &w, &h, &ch)) {
			printf("not a valid image file: %s\n", expanded);
			free(expanded);
			return -EIO;
		}
		strncpy(ctx->image_path, expanded, sizeof(ctx->image_path) - 1);
		image_render_terminal(expanded);
		printf("image loaded: %s (%dx%d, %d channels)\n", expanded, w, h, ch);
		free(expanded);
		return 0;
	}
	if (strncmp(input, "/video", 6) == 0) {
		printf("video injection not yet available (M3)\n");
		return 0;
	}
	if (strncmp(input, "/export", 7) == 0) {
		printf("use /save [format]\n");
		return 0;
	}

	char input_buf[8192];
	const char *effective_input = input;
	if (ctx->image_path[0]) {
		int n = snprintf(input_buf, sizeof(input_buf),
				 "[Image: %s]\n%s", ctx->image_path, input);
		if (n > 0 && (size_t)n < sizeof(input_buf))
			effective_input = input_buf;
		ctx->image_path[0] = '\0';
	}

	react_run(ctx->react, effective_input, output_callback, ctx);
	int user_tokens = tokenizer_count(ctx->tokenizer, effective_input);
	message_add(&ctx->database, ctx->current_session.id, "user", effective_input, user_tokens);
	session_update_tokens(&ctx->database, ctx->current_session.id, user_tokens);
	ctx->streaming = 0;
	return 0;
}

void cli_print_help(void)
{
	printf("multi-agent commands:\n");
	printf("  /help [cmd]      - Show help\n");
	printf("  /new [name]      - Create new session\n");
	printf("  /switch <name|id>- Switch session\n");
	printf("  /list            - List sessions\n");
	printf("  /rename <new>    - Rename current session\n");
	printf("  /delete <name|id>- Delete session\n");
	printf("  /history [n]     - Show recent messages\n");
	printf("  /model [name]    - View/switch model\n");
	printf("  /trace           - Show ReAct trace\n");
	printf("  /context         - Show token usage\n");
	printf("  /compress        - Manual compress\n");
	printf("  /save [fmt]      - Export session (md/json/txt)\n");
	printf("  /export <fmt>    - Alias for /save\n");
	printf("  /image <path>    - Inject image into current message (M2)\n");
	printf("  /video <path>    - Inject video (M3)\n");
	printf("  /config          - View config\n");
	printf("  /skill list       - List tools/skills\n");
	printf("  /quit            - Exit\n");
}

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
	db_close(&ctx->database);
	log_info("cli shutdown complete");
}