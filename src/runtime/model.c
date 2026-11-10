#include "runtime/model.h"

#include "runtime/runtime_internal.h"
#include "agent/agent_control.h"
#include "config/edit.h"
#include "util/cleanup.h"
#include "util/file.h"
#include "util/json.h"
#include "util/error.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

static void model_lock_cleanup(int *fd)
{
	if (*fd >= 0)
		close(*fd);
}

static int model_save_mode(struct runtime *runtime, int ultra)
{
	const char *path = runtime->context.config_path;
	if (!*path)
		return -ENOENT;
	morph_buf_t lock_path MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_buf_init(&lock_path, strlen(path) + 6);
	if (rc == 0)
		rc = morph_buf_printf(&lock_path, "%s.lock", path);
	if (rc != 0)
		return rc;
	int fd MORPH_CLEANUP(model_lock_cleanup) =
		open(morph_buf_cstr_borrow(&lock_path), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0)
		return morph_error_from_errno(errno);
	if (flock(fd, LOCK_EX | LOCK_NB) != 0)
		return morph_error_from_errno(errno);
	char *before MORPH_CLEANUP(morph_charp_cleanup) = file_read_all_copy(path, NULL);
	char *after MORPH_CLEANUP(morph_charp_cleanup) = NULL;
	if (!before)
		return errno ? morph_error_from_errno(errno) : -EIO;
	rc = config_text_merge_copy(before,
				    ultra ? "[agents]\nmulti_agent_mode = \"proactive\"\n"
					  : "[agents]\nmulti_agent_mode = \"explicit\"\n",
				    &after);
	struct config_validation_error error = {0};
	if (rc == 0)
		rc = config_validate_text(after, &error);
	if (rc == 0)
		rc = file_write_atomic(path, after, strlen(after));
	return rc;
}

int runtime_model_get_ultra(struct runtime *runtime, int *ultra)
{
	if (!runtime || !ultra)
		return -EINVAL;
	struct runtime_context *ctx = &runtime->context;
	if (pthread_mutex_trylock(&ctx->execution_lock) != 0)
		return -EBUSY;
	*ultra = 0;
	if (ctx->agents && ctx->config.agents.enabled) {
		const char *mode = ctx->config.agents.multi_agent_mode;
		*ultra = !strcmp(mode, "proactive");
		if (!strcmp(mode, "auto")) {
			cJSON *extra MORPH_CLEANUP(morph_json_cleanup) =
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
	pthread_mutex_unlock(&ctx->execution_lock);
	return 0;
}

int runtime_model_set_ultra(struct runtime *runtime, int ultra)
{
	if (!runtime || (ultra != 0 && ultra != 1))
		return -EINVAL;
	struct runtime_context *ctx = &runtime->context;
	if (pthread_mutex_trylock(&ctx->execution_lock) != 0)
		return -EBUSY;
	int rc = 0;
	if (ultra && (!ctx->agents || !ctx->config.agents.enabled))
		rc = -ENOTSUP;
	if (rc == 0 && agent_control_active(ctx->agents, 0))
		rc = -EBUSY;
	if (rc == 0)
		rc = model_save_mode(runtime, ultra);
	if (rc == 0) {
		strcpy(ctx->config.agents.multi_agent_mode, ultra ? "proactive" : "explicit");
		agent_control_set_proactive(ctx->agents, ultra);
	}
	pthread_mutex_unlock(&ctx->execution_lock);
	return rc;
}
