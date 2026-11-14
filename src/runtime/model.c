#include "runtime/model.h"

#include <stdlib.h>
#include "runtime/runtime_internal.h"
#include "agent/agent_control.h"
#include "config/edit.h"
#include "nb/cleanup.h"
#include "nb/file.h"
#include "cJSON.h"
#include "errors.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

static void model_lock_cleanup(nb_file_lock **lock)
{
	nb_file_lock_release(*lock);
	*lock = NULL;
}

static int model_save_mode(struct runtime *runtime, int ultra)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};

	const char *path = runtime->context.config_path;
	if (!*path)
		return NB_ERR_NOT_FOUND;
	nb_buf lock_path NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&lock_path),
		  nb_buf_reserve(&lock_path, strlen(path) + 6) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = (nb_buf_printf(&lock_path, "%s.lock", path) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	nb_file_lock *lock NB_CLEANUP(model_lock_cleanup) = NULL;
	rc = nb_file_lock_acquire(nb_buf_cstr(&lock_path), true, false, &lock, NULL);
	if (rc != NB_OK)
		return rc;
	rc = nb_file_read_all(path, &file_value_1, 0, NULL);
	if (rc != NB_OK)
		return rc;
	char *after NB_CLEANUP(nb_charp_cleanup) = NULL;
	rc = config_text_merge_copy(nb_buf_cstr(&file_value_1),
				    ultra ? "[agents]\nmulti_agent_mode = \"proactive\"\n"
					  : "[agents]\nmulti_agent_mode = \"explicit\"\n",
				    &after);
	struct config_validation_error error = {0};
	if (rc == 0)
		rc = config_validate_text(after, &error);
	if (rc == 0)
		rc = nb_file_write_all_atomic(
			path, after, strlen(after),
			&(nb_file_write_options){.new_file_mode = 0600, .sync_dir = true}, NULL);
	return rc;
}

int runtime_model_get_ultra(struct runtime *runtime, int *ultra)
{
	if (!runtime || !ultra)
		return NB_ERR_INVAL;
	struct runtime_context *ctx = &runtime->context;
	if (nb_mutex_trylock(ctx->execution_lock) != 0)
		return NB_ERR_BUSY;
	*ultra = 0;
	if (ctx->agents && ctx->config.agents.enabled) {
		const char *mode = ctx->config.agents.multi_agent_mode;
		*ultra = !strcmp(mode, "proactive");
		if (!strcmp(mode, "auto")) {
			cJSON *extra NB_CLEANUP(cJSON_DeleteOwner) =
				cJSON_Parse(ctx->llm ? ctx->llm->extra_body_json : "{}");
			const char *effort = cJSON_GetStringValue(
				cJSON_GetObjectItemCaseSensitive(extra, "reasoning_effort"));
			if (!effort)
				effort = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
					cJSON_GetObjectItemCaseSensitive(extra, "reasoning"),
					"effort"));
			*ultra = effort && !strcmp(effort, "ultra");
		}
	}
	nb_mutex_unlock(ctx->execution_lock);
	return 0;
}

int runtime_model_set_ultra(struct runtime *runtime, int ultra)
{
	if (!runtime || (ultra != 0 && ultra != 1))
		return NB_ERR_INVAL;
	struct runtime_context *ctx = &runtime->context;
	if (nb_mutex_trylock(ctx->execution_lock) != 0)
		return NB_ERR_BUSY;
	int rc = 0;
	if (ultra && (!ctx->agents || !ctx->config.agents.enabled))
		rc = NB_ERR_UNSUPPORTED;
	if (rc == 0 && agent_control_active(ctx->agents, 0))
		rc = NB_ERR_BUSY;
	if (rc == 0)
		rc = model_save_mode(runtime, ultra);
	if (rc == 0) {
		strcpy(ctx->config.agents.multi_agent_mode, ultra ? "proactive" : "explicit");
		agent_control_set_proactive(ctx->agents, ultra);
	}
	nb_mutex_unlock(ctx->execution_lock);
	return rc;
}
