#include "cli/internal.h"

int cli_build_sync_config(struct cli_context *ctx, struct morph_sync_config *cfg)
{
	char source[PATH_MAX];
	char *expanded;
	char *slash;

	if (!ctx || !cfg)
		MORPH_RETURN(-EINVAL);
	memset(cfg, 0, sizeof(*cfg));
	cfg->enabled = ctx->config.sync.enabled;
	cfg->interval_seconds = ctx->config.sync.interval_seconds;
	cfg->retention_days = ctx->config.sync.retention_days;
	strncpy(cfg->sync_dir, ctx->config.sync.dir,
		sizeof(cfg->sync_dir) - 1);
	expanded = file_expand_path(default_config_path);
	if (expanded) {
		strncpy(source, expanded, sizeof(source) - 1);
		source[sizeof(source) - 1] = '\0';
		free(expanded);
	} else {
		strncpy(source, ctx->config_path, sizeof(source) - 1);
		source[sizeof(source) - 1] = '\0';
	}
	slash = strrchr(source, '/');
	if (slash)
		*slash = '\0';
	else
		strncpy(source, ".", sizeof(source) - 1);
	strncpy(cfg->source_dir, source, sizeof(cfg->source_dir) - 1);
	cfg->include_count = ctx->config.sync.include_count;
	if (cfg->include_count > MORPH_SYNC_INCLUDE_MAX)
		cfg->include_count = MORPH_SYNC_INCLUDE_MAX;
	for (int i = 0; i < cfg->include_count; i++) {
		strncpy(cfg->include[i], ctx->config.sync.include[i],
			MORPH_SYNC_INCLUDE_LEN_MAX - 1);
	}
	return 0;
}

int cli_sync_start(struct cli_context *ctx)
{
	struct morph_sync_config cfg;
	int rc;

	if (!ctx || ctx->sync_started)
		return 0;
	if (!ctx->config.sync.enabled)
		return 0;
	if (!ctx->config.sync.dir[0]) {
		log_warn("sync enabled without sync.dir");
		return -EINVAL;
	}
	rc = cli_build_sync_config(ctx, &cfg);
	if (rc != 0)
		return rc;
	rc = morph_sync_start(&ctx->sync_worker, &cfg);
	if (rc == 0)
		ctx->sync_started = 1;
	return rc;
}

void cli_sync_stop(struct cli_context *ctx)
{
	if (!ctx || !ctx->sync_started)
		return;
	morph_sync_stop(&ctx->sync_worker);
	ctx->sync_started = 0;
}

struct scheduled_task_event_sink cli_task_event_sink(
	struct cli_context *ctx)
{
	struct scheduled_task_event_sink events;

	events.cb = ctx ? ctx->event_cb : NULL;
	events.user_data = ctx ? ctx->event_user_data : NULL;
	return events;
}
void cli_process_due_tasks(struct cli_context *ctx)
{
	int ran = 0;
	int rc;

	if (!ctx || !ctx->database.handle)
		return;
	if (ctx->scheduler_started)
		return;
	struct scheduled_task_event_sink events = cli_task_event_sink(ctx);
	rc = scheduled_task_run_due_with_runner_events(
		&ctx->database, (int64_t)time(NULL), 50,
		cli_scheduled_task_runner, ctx, &ran, &events);
	if (rc == 0 && ran > 0) {
		cli_emit_background_event(ctx, "background.completed", "end",
					  "due tasks processed",
					  "scheduled_tasks", ran, 0);
		if (ctx->event_mode != CLI_EVENTS_JSON)
			printf(ANSI_DIM "[tasks] processed %d due task%s; "
			       "check /inbox" ANSI_RESET "\n",
			       ran, ran == 1 ? "" : "s");
	} else if (rc != 0) {
		cli_emit_background_event(ctx, "background.failed", "failed",
					  "due task processing failed",
					  "scheduled_tasks", -1, rc);
		log_warn("failed to process due tasks: %s", morph_strerror(rc));
	}
}

static char *scheduled_task_prompt(const struct scheduled_task *task)
{
	cJSON *payload;
	cJSON *prompt;
	char *out = NULL;

	if (!task || !task->payload_json)
		return NULL;
	payload = cJSON_Parse(task->payload_json);
	if (!payload)
		return NULL;
	prompt = cJSON_GetObjectItem(payload, "prompt");
	if (cJSON_IsString(prompt) && prompt->valuestring)
		out = strdup(prompt->valuestring);
	cJSON_Delete(payload);
	return out;
}

int cli_scheduled_task_runner(const struct scheduled_task *task,
				     struct scheduled_task_action_result *result,
				     void *user_data)
{
	struct cli_context *ctx = user_data;
	struct session previous_session;
	struct session run_session;
	morph_buf_t prompt;
	char session_name[256];
	char *task_prompt;
	const char *answer;
	struct memory_options mem_opts;
	struct agent_session_runtime runtime;
	struct agent_turn turn;
	struct agent_turn_input turn_input;
	int prompt_ready = 0;
	int locked = 0;
	int rc;

	if (!ctx || !ctx->react || !task || !result)
		MORPH_RETURN(-EINVAL);
	task_prompt = scheduled_task_prompt(task);
	if (!task_prompt)
		MORPH_RETURN(-EINVAL);
	rc = morph_buf_init(&prompt, 1024);
	if (rc != 0) {
		free(task_prompt);
		return rc;
	}
	prompt_ready = 1;
	rc = morph_buf_printf(&prompt,
		"Run this scheduled task now.\n\n"
		"Task title: %s\n\n"
		"Task prompt:\n%s\n\n"
		"Return the result to the user as a concise notification. "
		"Do not ask follow-up questions; if required information is "
		"missing, explain what is missing and what should be changed "
		"on the task.",
		task->title, task_prompt);
	free(task_prompt);
	if (rc != 0)
		goto out;

	pthread_mutex_lock(&ctx->react_lock);
	locked = 1;
	previous_session = ctx->current_session;
	snprintf(session_name, sizeof(session_name), "Task #%lld: %.220s",
		 (long long)task->id, task->title);
	rc = session_create(&ctx->database, session_name,
			    ctx->config.models.text.model, &run_session);
	if (rc != 0)
		goto restore;
	session_ensure_display_id(&ctx->database, &run_session);
	ctx->current_session = run_session;
	mem_opts = cli_memory_options(ctx);
	memset(&runtime, 0, sizeof(runtime));
	runtime.db = &ctx->database;
	runtime.session_id = run_session.id;
	runtime.react = ctx->react;
	runtime.memory_options = &mem_opts;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS;
	memset(&turn_input, 0, sizeof(turn_input));
	turn_input.model_input = morph_buf_cstr(&prompt);
	rc = agent_turn_begin(&turn, &runtime, &turn_input);
	if (rc != 0)
		goto restore;

	rc = react_run(ctx->react, morph_buf_cstr(&prompt), NULL, NULL);
	answer = ctx->react->final_answer ? ctx->react->final_answer : "";
	result->session_id = run_session.id;
	if (rc == 0) {
		result->body = strdup(answer);
		if (!result->body)
			rc = -ENOMEM;
	} else {
		result->error = strdup(answer[0] ? answer : morph_strerror(rc));
		if (!result->error)
			rc = -ENOMEM;
	}
	agent_turn_finish(&turn, NULL);

restore:
	ctx->current_session = previous_session;
	session_load_history(ctx);
	pthread_mutex_unlock(&ctx->react_lock);
	locked = 0;

out:
	if (locked)
		pthread_mutex_unlock(&ctx->react_lock);
	if (prompt_ready)
		morph_buf_cleanup(&prompt);
	return rc;
}

static void cli_redisplay_prompt(struct cli_context *ctx)
{
	if (!ctx || ctx->event_mode == CLI_EVENTS_JSON)
		return;
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
		return;
#ifdef HAVE_READLINE
	rl_on_new_line();
	rl_forced_update_display();
#else
	printf(ANSI_GREEN "[%s]" ANSI_RESET " $ ",
	       ctx->current_session.display_id);
	fflush(stdout);
#endif
}

static void cli_print_task_notification(struct cli_context *ctx,
					const struct notification *notification)
{
	if (!notification)
		return;
	flockfile(stdout);
	printf("\n" ANSI_BOLD ANSI_CYAN "[task]" ANSI_RESET " %s\n",
	       notification->title);
	if (notification->body && notification->body[0])
		printf("%s\n", notification->body);
	printf(ANSI_DIM "stored in /inbox as #%lld" ANSI_RESET "\n",
	       (long long)notification->id);
	fflush(stdout);
	funlockfile(stdout);
	cli_redisplay_prompt(ctx);
}

static void *cli_scheduler_main(void *arg)
{
	struct cli_context *ctx = arg;
	struct db scheduler_db;
	int opened = 0;

	memset(&scheduler_db, 0, sizeof(scheduler_db));
	if (!ctx || !ctx->database.path[0])
		return NULL;
	if (db_open(&scheduler_db, ctx->database.path) == 0 &&
	    db_init_schema(&scheduler_db) == 0) {
		opened = 1;
	} else {
		log_warn("task scheduler failed to open database");
	}

	while (opened) {
		struct notification *notifications = NULL;
		int count = 0;
		int rc;

		pthread_mutex_lock(&ctx->scheduler_lock);
		if (ctx->scheduler_stop) {
			pthread_mutex_unlock(&ctx->scheduler_lock);
			break;
		}
		pthread_mutex_unlock(&ctx->scheduler_lock);

		struct scheduled_task_event_sink events =
			cli_task_event_sink(ctx);
		rc = scheduled_task_run_due_collect_with_runner_events(
			&scheduler_db, (int64_t)time(NULL), 50,
			cli_scheduled_task_runner, ctx,
			&notifications, &count, &events);
		if (rc == 0) {
			for (int i = 0; i < count; i++)
				cli_print_task_notification(ctx,
							    &notifications[i]);
			notification_free_list(notifications, count);
		} else {
			log_warn("task scheduler failed: %s", morph_strerror(rc));
		}

		pthread_mutex_lock(&ctx->scheduler_lock);
		if (!ctx->scheduler_stop) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 1;
			(void)pthread_cond_timedwait(&ctx->scheduler_cond,
						     &ctx->scheduler_lock,
						     &ts);
		}
		if (ctx->scheduler_stop) {
			pthread_mutex_unlock(&ctx->scheduler_lock);
			break;
		}
		pthread_mutex_unlock(&ctx->scheduler_lock);
	}

	if (opened)
		db_close(&scheduler_db);
	return NULL;
}

int cli_scheduler_start(struct cli_context *ctx)
{
	int rc;

	if (!ctx || ctx->scheduler_started)
		return 0;
	cli_emit_background_event(ctx, "background.started", "begin",
				  "task scheduler starting",
				  "task_scheduler", -1, 0);
	rc = pthread_mutex_init(&ctx->scheduler_lock, NULL);
	if (rc != 0) {
		cli_emit_background_event(ctx, "background.failed", "failed",
					  "task scheduler failed",
					  "task_scheduler", -1, -rc);
		return -rc;
	}
	rc = pthread_cond_init(&ctx->scheduler_cond, NULL);
	if (rc != 0) {
		pthread_mutex_destroy(&ctx->scheduler_lock);
		cli_emit_background_event(ctx, "background.failed", "failed",
					  "task scheduler failed",
					  "task_scheduler", -1, -rc);
		return -rc;
	}
	ctx->scheduler_stop = 0;
	rc = pthread_create(&ctx->scheduler_thread, NULL,
			    cli_scheduler_main, ctx);
	if (rc != 0) {
		pthread_cond_destroy(&ctx->scheduler_cond);
		pthread_mutex_destroy(&ctx->scheduler_lock);
		cli_emit_background_event(ctx, "background.failed", "failed",
					  "task scheduler failed",
					  "task_scheduler", -1, -rc);
		return -rc;
	}
	ctx->scheduler_started = 1;
	cli_emit_background_event(ctx, "background.ready", "ready",
				  "task scheduler ready",
				  "task_scheduler", -1, 0);
	return 0;
}

void cli_scheduler_stop(struct cli_context *ctx)
{
	if (!ctx || !ctx->scheduler_started)
		return;
	cli_emit_background_event(ctx, "background.stopping", "begin",
				  "task scheduler stopping",
				  "task_scheduler", -1, 0);
	pthread_mutex_lock(&ctx->scheduler_lock);
	ctx->scheduler_stop = 1;
	pthread_cond_signal(&ctx->scheduler_cond);
	pthread_mutex_unlock(&ctx->scheduler_lock);
	pthread_join(ctx->scheduler_thread, NULL);
	pthread_cond_destroy(&ctx->scheduler_cond);
	pthread_mutex_destroy(&ctx->scheduler_lock);
	ctx->scheduler_started = 0;
	ctx->scheduler_stop = 0;
	cli_emit_background_event(ctx, "background.completed", "end",
				  "task scheduler stopped",
				  "task_scheduler", -1, 0);
}
