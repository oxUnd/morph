#include "sapi/fastcgi/services/turn_job.h"
#include "sapi/fastcgi/services/turn_job_internal.h"
#include "sapi/fastcgi/agent_bridge.h"
#include "sapi/fastcgi/session_store.h"
#include "sapi/fastcgi/storage/sessions.h"
#include "sapi/fastcgi/storage/events.h"
#include "sapi/fastcgi/storage/quota.h"
#include "sapi/fastcgi/event_sink.h"
#include "util/json.h"
#include "util/error.h"
#include "util/child_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sapi/fastcgi/adapters/turn_events.h"
#include "sapi/fastcgi/adapters/turn_interactions.h"
#include "sapi/fastcgi/presentation/media_refs.h"
#include "runtime/runtime.h"
#include "session.h"

static int bridge_memory_session_visible(const char *display_id, const char *user_id,
					 void *user_data)
{
	struct session_store *store = user_data;

	if (!store || !display_id || !user_id)
		return 0;
	return store_session_owned_by(store, display_id, user_id);
}

static void *turn_heartbeat_thread(void *arg)
{
	struct fcgi_turn_job *j = (struct fcgi_turn_job *)arg;
	const struct timespec tick = {1, 0};

	while (!atomic_load(&j->heartbeat_stop)) {
		store_turn_heartbeat(j->store, j->turn_id);
		for (int i = 0; i < 15 && !atomic_load(&j->heartbeat_stop); i++)
			nanosleep(&tick, NULL);
	}
	return NULL;
}

static void *turn_thread(void *arg)
{
	struct fcgi_turn_job *j = (struct fcgi_turn_job *)arg;
	struct session sess = {0};
	struct runtime *runtime = NULL;
	struct runtime_request request;
	struct runtime_result result;
	int run_rc;
	int ok = 0;
	int64_t waited_ms = 0;

	atomic_init(&j->heartbeat_stop, 0);
	if (pthread_create(&j->heartbeat_thread, NULL, turn_heartbeat_thread, j) == 0)
		j->heartbeat_started = 1;
	memset(&result, 0, sizeof(result));
	if (session_get_by_display_id(&j->store->db, j->session_id, &sess) == 0) {
		memset(&request, 0, sizeof(request));
		request.session_id = sess.id;
		request.model_input = j->input ? j->input : "";
		request.stored_user_input = request.model_input;
		request.turn_id = j->turn_id;
		request.render_assistant = fcgi_media_refs_store;
		request.render_user_data = j;
		request.user_id = j->user_id;
		request.restrict_memory_to_user = 1;
		request.memory_visible_fn = bridge_memory_session_visible;
		request.memory_visible_user_data = j->store;
		request.event_cb = fcgi_turn_events_callback;
		request.event_user_data = j;
		request.hitl_cb = fcgi_turn_interactions_hitl;
		request.hitl_user_data = j;
		request.override_hitl = 1;
		request.ask_user_fn = fcgi_turn_ask_user;
		request.ask_user_user_data = j;
		request.override_ask_user = 1;
		request.operation_approval_fn = fcgi_turn_operation_approval;
		request.operation_approval_user_data = j;
		request.override_operation_approval = 1;
		request.action_drain_fn = fcgi_turn_interactions_drain;
		request.action_drain_user_data = j;
		request.override_action_drain = 1;
		request.turn_flags = AGENT_TURN_DEFAULT_FLAGS;

		run_rc = fcgi_bridge_turn_acquire(j->bridge, j->ticket, &runtime);
		j->ticket = NULL;
		if (run_rc != 0) {
			struct json *err = json_object();
			char *json = NULL;

			if (err) {
				json_add_string(err, "turn_id", j->turn_id);
				json_add_string(err, "message", morph_strerror(run_rc));
				json_add_bool(err, "retryable",
					      run_rc == -ETIMEDOUT || run_rc == -EAGAIN);
				json = json_stringify(err);
				json_free(err);
			}
			events_publish(j->store, j->session_id, "error",
				       json ? json : "{\"message\":\"turn failed\"}");
			free(json);
			goto out;
		}
		waited_ms = morph_io_now_ms() - j->enqueued_ms;
		{
			struct json *start = json_object();
			char *json = NULL;

			if (start) {
				json_add_string(start, "phase", "begin");
				json_add_string(start, "turn_id", j->turn_id);
				json_add_number(start, "queue_position", j->queue_position);
				json_add_number(start, "waited_ms", (double)waited_ms);
				json = json_stringify(start);
				json_free(start);
			}
			events_publish(j->store, j->session_id, "turn_start",
				       json ? json : "{\"phase\":\"begin\"}");
			free(json);
		}
		run_rc = runtime_execute_turn(runtime, &request, &result);
		fcgi_bridge_runtime_release(j->bridge, runtime);
		runtime = NULL;
		if (run_rc != 0) {
			struct json *err = json_object();
			char *json = NULL;

			if (err) {
				json_add_string(err, "turn_id", j->turn_id);
				json_add_string(err, "message", morph_strerror(run_rc));
				json = json_stringify(err);
				json_free(err);
			}
			events_publish(j->store, j->session_id, "error",
				       json ? json : "{\"message\":\"turn failed\"}");
			free(json);
			goto out;
		}
		if (!j->final_sent) {
			char *fallback = fcgi_media_refs_summary(j);
			if (!fallback && result.final_text)
				fallback = fcgi_media_refs_render(j, result.final_text);
			if (fallback && *fallback) {
				event_sink_final_turn(j->store, j->session_id, j->turn_id,
						      fallback);
				j->final_sent = 1;
			}
			free(fallback);
		}
		ok = 1;
	} else {
		struct json *err = json_object();
		char *json = NULL;

		if (err) {
			json_add_string(err, "turn_id", j->turn_id);
			json_add_string(err, "message", "session not found");
			json = json_stringify(err);
			json_free(err);
		}
		events_publish(j->store, j->session_id, "error",
			       json ? json : "{\"message\":\"session not found\"}");
		free(json);
		goto out;
	}

out:
	if (j->heartbeat_started) {
		atomic_store(&j->heartbeat_stop, 1);
		pthread_join(j->heartbeat_thread, NULL);
		j->heartbeat_started = 0;
	}
	if (j->ticket) {
		fcgi_bridge_turn_cancel(j->bridge, j->ticket);
		j->ticket = NULL;
	}
	fcgi_bridge_runtime_release(j->bridge, runtime);
	{
		struct json *end = json_object();
		char *json = NULL;

		if (end) {
			json_add_string(end, "phase", "done");
			json_add_string(end, "turn_id", j->turn_id);
			json_add_string(end, "status", ok ? "ok" : "error");
			json = json_stringify(end);
			json_free(end);
		}
		events_publish(j->store, j->session_id, "turn_end",
			       json ? json : "{\"phase\":\"done\"}");
		free(json);
	}
	store_quota_end_turn(j->store, j->turn_id);
	fcgi_bridge_turn_end(j->bridge);
	free(j->action_payload);
	free(j->pending_payload);
	free(j->input);
	free(j);
	return NULL;
}

static void turn_job_publish_abort(struct fcgi_turn_job *j, const char *message)
{
	struct json *err = json_object();
	struct json *end = json_object();
	char *err_json = NULL;
	char *end_json = NULL;

	if (err) {
		json_add_string(err, "turn_id", j->turn_id);
		json_add_string(err, "message", message ? message : "turn failed");
		err_json = json_stringify(err);
		json_free(err);
	}
	events_publish(j->store, j->session_id, "error",
		       err_json ? err_json : "{\"message\":\"turn failed\"}");
	free(err_json);

	if (end) {
		json_add_string(end, "phase", "done");
		json_add_string(end, "turn_id", j->turn_id);
		json_add_string(end, "status", "error");
		end_json = json_stringify(end);
		json_free(end);
	}
	events_publish(j->store, j->session_id, "turn_end",
		       end_json ? end_json : "{\"phase\":\"done\"}");
	free(end_json);
}

int fcgi_turn_job_submit(struct fcgi_bridge *bridge, struct session_store *store,
			 const char *session_id, const char *user_id, const char *input,
			 struct fcgi_turn_receipt *receipt)
{
	struct fcgi_turn_job *job;
	pthread_t thread;
	int rc;

	if (!bridge || !store || !session_id || !user_id || !input || !receipt)
		return -EINVAL;
	*receipt = (struct fcgi_turn_receipt){.failure = FCGI_TURN_FAILURE_INTERNAL};
	job = calloc(1, sizeof(*job));
	if (!job)
		return -ENOMEM;
	job->bridge = bridge;
	job->store = store;
	snprintf(job->session_id, sizeof(job->session_id), "%s", session_id);
	snprintf(job->user_id, sizeof(job->user_id), "%s", user_id);
	rc = store_quota_begin_turn(store, user_id, session_id, job->turn_id);
	if (rc) {
		free(job);
		receipt->failure = rc == -EDQUOT   ? FCGI_TURN_FAILURE_DAILY_QUOTA
				   : rc == -EAGAIN ? FCGI_TURN_FAILURE_CONCURRENCY_QUOTA
						   : FCGI_TURN_FAILURE_INTERNAL;
		return rc;
	}
	job->input = strdup(input);
	if (!job->input) {
		store_quota_end_turn(store, job->turn_id);
		free(job);
		return -ENOMEM;
	}
	rc = fcgi_bridge_turn_enqueue(bridge, session_id, &job->ticket, &job->queue_position);
	if (rc) {
		store_quota_end_turn(store, job->turn_id);
		free(job->input);
		free(job);
		receipt->failure =
			rc == -EAGAIN ? FCGI_TURN_FAILURE_QUEUE_FULL : FCGI_TURN_FAILURE_SHUTDOWN;
		return rc;
	}
	job->enqueued_ms = morph_io_now_ms();
	struct json *queued = json_object();
	char *json = NULL;

	if (queued) {
		json_add_string(queued, "turn_id", job->turn_id);
		json_add_number(queued, "position", job->queue_position);
		json = json_stringify(queued);
		json_free(queued);
	}
	events_publish(store, session_id, "turn_queued", json ? json : "{\"position\":1}");
	free(json);
	receipt->failure = FCGI_TURN_FAILURE_SHUTDOWN;
	rc = fcgi_bridge_turn_begin(bridge);
	if (!rc) {
		snprintf(receipt->turn_id, sizeof(receipt->turn_id), "%s", job->turn_id);
		receipt->queue_position = job->queue_position;
		receipt->failure = FCGI_TURN_FAILURE_THREAD;
		int error = pthread_create(&thread, NULL, turn_thread, job);

		if (!error) {
			receipt->failure = FCGI_TURN_FAILURE_NONE;
			pthread_detach(thread);
			return 0;
		}
		rc = -error;
		fcgi_bridge_turn_end(bridge);
	}
	fcgi_bridge_turn_cancel(bridge, job->ticket);
	turn_job_publish_abort(job, morph_strerror(rc));
	store_quota_end_turn(store, job->turn_id);
	free(job->input);
	free(job);
	return rc;
}
