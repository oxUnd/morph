#ifndef MORPH_CLI_APPLICATION_H
#define MORPH_CLI_APPLICATION_H

#include "config/config.h"
#include "runtime/runtime.h"
#include "sapi/cli/services/recap.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include "sapi/cli/models/composer.h"
#include "sapi/cli/models/agents.h"
#include <nb/signal.h>
#include <nothing/tui/application.h>
#include <nothing/tui/view_controller.h>
#include <nothing/tui/screen.h>
#include "sapi/cli/models/presentation.h"
#include <stdint.h>
#include <stdio.h>

struct nb_log;

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
struct cli_subagents_session;
struct cli_sessions;

struct cli_application {
	struct nt_application base;
	struct cli_chat_page *chat;
	struct cli_presenter_state *presenter;
	struct cli_input_state *input;
	struct nb_log *logger;
	struct cli_output output;
	struct cli_composer_model composer;
	nb_signal_listener *signals;
	int signals_ready;
	nb_atomic_int terminate_requested;
	nb_atomic_int interrupted;
	nb_atomic_int resized;

	struct cli_recap_state recap;
	struct runtime *runtime;
	struct cli_sessions *sessions;
	int (*input_execute)(struct cli_application *, const char *input);
	struct config_file_error startup_error;
	struct cli_event_queue *events;
	int application_command;
	size_t modal_count;
	struct cli_feed *terminal;
	struct cli_command_job *input_job;
	struct cli_transcript_model *transcript;
	struct cli_agents_model agents;
	int agents_refreshing;
	struct cli_turn_details *details;
	struct cli_jobs *jobs;
	struct cli_subagents_session *subagents;
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

int cli_application_init(struct cli_application *ctx, struct nb_log *logger,
			 const char *config_path, const char *db_path, const char *workdir,
			 const char *session_name, const char *resume_ref,
			 enum cli_presentation_mode mode);
void cli_application_run(struct cli_application *ctx);
void cli_application_run_once(struct cli_application *ctx, const char *prompt);
void cli_application_shutdown(struct cli_application *ctx);
struct nt_application *cli_application_tui_borrow(struct cli_application *);
/* Owner-thread publication after a synchronous command updates output. */
int cli_application_flush_output(struct cli_application *ctx);
struct cli_application *cli_application_from_controller(const struct nt_view_controller *);
struct cli_application *cli_application_from_view(const struct nt_view *);
void cli_application_controller_cleanup(struct nt_view_controller **);
/* Present a cli_application_panel_class instance. NT clears the owner on acceptance. */
int cli_application_show_page(struct cli_application *, struct nt_view_controller **, int tick_ms);
int cli_application_prompt_deferred(const struct cli_application *);
extern const struct nt_view_controller_class cli_application_panel_class;
void cli_application_panel_finished(struct cli_application *, int result);

#endif
