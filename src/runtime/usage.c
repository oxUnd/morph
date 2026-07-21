#include "models/llm.h"
#include "runtime/usage.h"

#include "cJSON.h"
#include "credits.h"

#include <stdlib.h>
#include <string.h>

void *runtime_usage_bind(void *user_data)
{
	void *previous = model_get_usage_user_data();

	model_set_usage_user_data(user_data);
	return previous;
}

void runtime_usage_restore(void *previous)
{
	model_set_usage_user_data(previous);
}

int runtime_model_usage_is_billable(const struct model_usage *usage)
{
	return usage && (usage->input_tokens > 0 || usage->output_tokens > 0 ||
		usage->image_units > 0 || usage->video_seconds > 0);
}

char *runtime_model_usage_metadata(const struct model_usage *usage)
{
	cJSON *obj;
	char *json;

	if (!usage)
		return NULL;
	obj = cJSON_CreateObject();
	if (!obj)
		return NULL;
	if (usage->response_id[0])
		cJSON_AddStringToObject(obj, "response_id",
					usage->response_id);
	if (usage->model[0])
		cJSON_AddStringToObject(obj, "actual_model", usage->model);
	if (usage->finish_reason[0])
		cJSON_AddStringToObject(obj, "finish_reason",
					usage->finish_reason);
	if (usage->system_fingerprint[0])
		cJSON_AddStringToObject(obj, "system_fingerprint",
					usage->system_fingerprint);
	if (usage->usage_source[0])
		cJSON_AddStringToObject(obj, "usage_source",
					usage->usage_source);
	if (usage->created > 0)
		cJSON_AddNumberToObject(obj, "created",
					(double)usage->created);
	if (usage->total_tokens > 0)
		cJSON_AddNumberToObject(obj, "total_tokens",
					(double)usage->total_tokens);
	if (usage->cached_tokens > 0)
		cJSON_AddNumberToObject(obj, "cached_tokens",
					(double)usage->cached_tokens);
	if (usage->reasoning_tokens > 0)
		cJSON_AddNumberToObject(obj, "reasoning_tokens",
					(double)usage->reasoning_tokens);
	if (usage->audio_tokens > 0)
		cJSON_AddNumberToObject(obj, "audio_tokens",
					(double)usage->audio_tokens);
	if (usage->image_tokens > 0)
		cJSON_AddNumberToObject(obj, "image_tokens",
					(double)usage->image_tokens);
	json = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	return json;
}

int runtime_record_model_usage(struct db *db,
			       const struct config *config,
			       const char *session_id,
			       const struct model_usage *usage)
{
	struct credit_event event;
	char *metadata;
	int rc;

	if (!db || !config || !session_id || !usage)
		return -EINVAL;
	if (!runtime_model_usage_is_billable(usage))
		return 0;
	metadata = runtime_model_usage_metadata(usage);
	memset(&event, 0, sizeof(event));
	event.user_id = "local";
	event.session_id = session_id;
	event.kind = usage->kind[0] ? usage->kind : "model_text";
	event.provider = usage->provider[0] ? usage->provider :
		config->models.text.provider;
	event.model = usage->model[0] ? usage->model :
		config->models.text.model;
	event.input_tokens = usage->input_tokens;
	event.cached_tokens = usage->cached_tokens;
	event.output_tokens = usage->output_tokens;
	event.image_units = usage->image_units;
	event.video_seconds = usage->video_seconds;
	event.metadata_json = metadata;
	rc = credit_record_event(db, &config->credits, &event, NULL);
	free(metadata);
	return rc;
}
