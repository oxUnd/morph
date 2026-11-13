#ifndef MORPH_CLI_APPLICATION_H
#define MORPH_CLI_APPLICATION_H

struct morph_log;
#include "config/config.h"
#include "runtime/runtime.h"
#include "sapi/cli/services/recap.h"
#include "util/buf.h"
#include "sapi/cli/models/composer.h"
#include "sapi/cli/models/agents.h"
#include "util/signals.h"
#include <morph/tui/application.h>
#include "sapi/cli/models/presentation.h"
#include <stdint.h>
#include <stdio.h>

struct cli_presenter_state;
struct cli_input_state;
struct cli_cancel_monitor;
struct cli_event_queue;
struct cli_feed;
struct cli_command_job;
struct cli_transcript_model;
struct cli_turn_details;
struct cli_jobs;
struct cli_chat_page;
struct cli_subagents_page;

struct cli_application {
	struct morph_tui_application base;
	struct cli_chat_page *chat;
	struct cli_presenter_state *presenter;
	struct cli_input_state *input;
	struct morph_log *logger;
	struct cli_output output;
	struct cli_composer_model composer;
	struct morph_signal_listener signals;
	int signals_ready;
	volatile sig_atomic_t terminate_requested;
	volatile sig_atomic_t interrupted;
	volatile sig_atomic_t resized;

	struct cli_recap_state recap;
	struct runtime *runtime;
	int (*input_execute)(struct cli_application *, const char *input);
	struct config_file_error startup_error;
	struct cli_event_queue *events;
	int application_command;
	struct cli_feed *terminal;
	struct cli_command_job *input_job;
	struct cli_transcript_model *transcript;
	struct cli_agents_model agents;
	struct cli_turn_details *details;
	struct cli_jobs *jobs;
	struct cli_subagents_page *subagents;
	struct cli_input_status input_status;
	int tool_details;
	enum cli_presentation_mode presentation_mode;
	int running;
	int session_auto_named;
	int resume_requested;
	int session_history_pending;
	int presentation_ready;
	int turn_active;
	struct cli_cancel_monitor *cancel_monitor;
	char image_path[PATH_MAX];
	char workdir[PATH_MAX];
	int trace_json;
	int pending_db_restore;
	uint64_t interaction_sequence;
	struct morph_sync_restore_plan db_restore_plan;
	morph_event_cb event_cb;
	void *event_user_data;
};

int cli_application_init(struct cli_application *ctx, struct morph_log *logger,
			 const char *config_path, const char *db_path, const char *workdir,
			 const char *session_name, const char *resume_ref,
			 enum cli_presentation_mode mode);
void cli_application_run(struct cli_application *ctx);
void cli_application_run_once(struct cli_application *ctx, const char *prompt);
void cli_application_shutdown(struct cli_application *ctx);
struct morph_tui_application *cli_application_tui_borrow(struct cli_application *);
int cli_application_present_screen(struct cli_application *, const struct morph_tui_screen_ops *,
				   void *data, void (*complete)(void *, int));
int cli_application_present_screen_with_flags(struct cli_application *,
					      const struct morph_tui_screen_ops *, void *data,
					      void (*complete)(void *, int), int flags);
void cli_application_panel_complete(struct cli_application *, void *data,
				    void (*complete)(void *, int), int result);

#endif
