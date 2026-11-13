#include "sapi/cli/models/subagents.h"
#include "nb/err.h"

#include "sapi/cli/models/transcript.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *cli_subagents_string(const cJSON *data, const char *key)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(data, key));
	return text ? text : "";
}

int cli_subagents_thread_create(const char *id, struct cli_subagents_thread **out)
{
	if (!id || !out || *out || strlen(id) >= sizeof((*out)->id))
		return NB_ERR_INVAL;
	struct cli_subagents_thread *thread = calloc(1, sizeof(*thread));
	if (!thread)
		return NB_ERR_NOMEM;
	memcpy(thread->id, id, strlen(id) + 1);
	struct cli_subagents_segment segment = {0};
	int rc = cli_transcript_model_create(&thread->transcript);
	if (rc == 0)
		rc = (nb_vec_init(&thread->segments, sizeof(segment)),
		      nb_vec_reserve(&thread->segments, 16) ? 0 : NB_ERR_NOMEM);
	if (rc != 0) {
		cli_subagents_thread_destroy(thread);
		return rc;
	}
	*out = thread;
	return 0;
}

void cli_subagents_thread_destroy(struct cli_subagents_thread *thread)
{
	if (!thread)
		return;
	cli_transcript_model_destroy(thread->transcript);
	struct cli_subagents_segment *segment;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(&thread->segments) &&
	     (segment = (struct cli_subagents_segment *)nb_vec_at(&thread->segments, _i_));
	     _i_++)
		nb_buf_cleanup(&segment->text);
	nb_vec_cleanup(&thread->segments);
	free(thread);
}

static int cli_subagents_append(struct cli_subagents_thread *thread,
				enum cli_subagents_segment_kind kind, const char *text, int begin)
{
	struct cli_subagents_segment *segment =
		thread->segments.len ? nb_vec_at(&thread->segments, thread->segments.len - 1)
				     : NULL;
	if (!segment || segment->kind != kind || begin) {
		segment = nb_vec_push(&thread->segments);
		if (!segment)
			return NB_ERR_NOMEM;
		memset(segment, 0, sizeof(*segment));
		segment->kind = kind;
		int rc = (nb_buf_init(&segment->text),
			  nb_buf_reserve(&segment->text, 256) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	return (nb_buf_append_cstr(&segment->text, text) ? 0 : NB_ERR_NOMEM);
}

int cli_subagents_thread_event(struct cli_subagents_thread *thread, const struct morph_event *event)
{
	if (!thread || !event || !event->name)
		return NB_ERR_INVAL;
	const char *id = cli_subagents_string(event->data, "agent_id");
	if (strcmp(id, thread->id))
		return 0;
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(event->data, "agent_event_sequence");
	int64_t sequence = cJSON_IsNumber(value) ? (int64_t)value->valuedouble : 0;
	if (sequence > 0 && sequence <= thread->sequence)
		return 0;
	const char *text = cli_subagents_string(event->data, "text");
	const char *name = event->name;
	int rc = 0;
	if (!strcmp(name, "react.turn.begin")) {
		thread->reasoning_open = 0;
		thread->content_open = 0;
		thread->content_seen = 0;
		rc = cli_subagents_append(thread, CLI_SUBAGENTS_USER, text, 1);
	} else if ((!strcmp(name, "react.reasoning.delta") ||
		    !strcmp(name, "react.thought.delta"))) {
		rc = cli_subagents_append(thread, CLI_SUBAGENTS_REASONING, text,
					  !thread->reasoning_open);
		thread->reasoning_open = 1;
	} else if (!strcmp(name, "react.thought.end") && *text) {
		struct cli_subagents_segment *last =
			thread->segments.len
				? nb_vec_at(&thread->segments, thread->segments.len - 1)
				: NULL;
		if (!last || last->kind != CLI_SUBAGENTS_REASONING ||
		    strcmp(nb_buf_cstr(&last->text), text))
			rc = cli_subagents_append(thread, CLI_SUBAGENTS_REASONING, text, 1);
		thread->reasoning_open = 0;
	} else if (!strcmp(name, "react.reasoning.end")) {
		thread->reasoning_open = 0;
	} else if (!strcmp(name, "react.content.delta") || !strcmp(name, "react.final.delta")) {
		rc = cli_subagents_append(thread, CLI_SUBAGENTS_ASSISTANT, text,
					  !thread->content_open);
		thread->content_open = 1;
		thread->content_seen = 1;
		thread->has_content = 1;
	} else if (!strcmp(name, "react.content.end")) {
		if (!thread->content_open && *text) {
			rc = cli_subagents_append(thread, CLI_SUBAGENTS_ASSISTANT, text, 1);
			thread->content_seen = 1;
			thread->has_content = 1;
		}
		thread->content_open = 0;
	} else if (!strcmp(name, "plan.updated")) {
		char *state NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(event->data);
		if (!state)
			return NB_ERR_NOMEM;
		rc = cli_subagents_append(thread, CLI_SUBAGENTS_PLAN, state, 1);
		thread->reasoning_open = 0;
		thread->content_open = 0;
	} else if (!strcmp(name, "react.final") && !thread->content_seen) {
		rc = cli_subagents_append(thread, CLI_SUBAGENTS_ASSISTANT, text, 1);
		thread->content_seen = 1;
		thread->has_content = 1;
	}
	struct cli_transcript_change change = {0};
	const cJSON *stamp = cJSON_GetObjectItemCaseSensitive(event->data, "agent_event_time_ms");
	double timestamp = cJSON_IsNumber(stamp) ? stamp->valuedouble / 1000.0 : 0.0;
	if (rc == 0)
		rc = cli_transcript_model_reduce(thread->transcript, event, timestamp, &change);
	if (rc == 0 && change.kind == CLI_TRANSCRIPT_TOOL_STARTED) {
		struct cli_subagents_segment *segment = nb_vec_push(&thread->segments);
		if (!segment)
			return NB_ERR_NOMEM;
		memset(segment, 0, sizeof(*segment));
		segment->kind = CLI_SUBAGENTS_TOOL;
		segment->tool_index = change.tool_index;
		thread->content_open = 0;
		thread->content_seen = 0;
		thread->reasoning_open = 0;
	}
	if (rc == 0 && sequence > thread->sequence)
		thread->sequence = sequence;
	return rc;
}

int cli_subagents_thread_replay(struct cli_subagents_thread *thread, const char *json)
{
	cJSON *record NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(json);
	if (!record)
		return NB_ERR_INVAL;
	struct morph_event event = {
		.type = MORPH_EVENT_BACKGROUND,
		.name = cli_subagents_string(record, "name"),
		.phase = cli_subagents_string(record, "phase"),
		.message = cli_subagents_string(record, "message"),
		.turn_id = cli_subagents_string(record, "turn_id"),
		.data = cJSON_GetObjectItemCaseSensitive(record, "data"),
	};
	const char *type = cli_subagents_string(record, "type");
	for (int i = MORPH_EVENT_STARTUP; i <= MORPH_EVENT_INTERACTION; i++) {
		if (!strcmp(type, morph_event_type_name((enum morph_event_type)i))) {
			event.type = (enum morph_event_type)i;
			break;
		}
	}
	return cli_subagents_thread_event(thread, &event);
}
