#include "img_qa.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "agent/tool_runtime.h"
#include "models/llm.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/error.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct img_qa_context {
	struct model *llm;
	struct tool_context *tctx;
};

static void img_qa_context_destroy(void *user_data)
{
	free(user_data);
}

static int img_qa_int_arg(cJSON *root, const char *name, int fallback,
			  int min, int max, struct tool_result *result)
{
	cJSON *item;
	int value;

	item = cJSON_GetObjectItem(root, name);
	if (!item)
		return fallback;
	if (!cJSON_IsNumber(item)) {
		(void)tool_result_json_errorf(result,
			"'%s' must be an integer", name);
		return -EINVAL;
	}
	value = item->valueint;
	if (value < min || value > max) {
		(void)tool_result_json_errorf(result,
			"'%s' must be between %d and %d", name, min, max);
		return -EINVAL;
	}
	return value;
}

static int img_qa_take_call_error(struct tool_result *result,
				  struct model *llm)
{
	cJSON *root;
	char *json;
	int rc;

	if (!result)
		MORPH_RETURN(-EINVAL);
	root = cJSON_CreateObject();
	if (!root)
		MORPH_RETURN(-ENOMEM);
	cJSON_AddStringToObject(root, "error", "image QA LLM call failed");
	if (llm && llm->last_error[0])
		cJSON_AddStringToObject(root, "detail", llm->last_error);
	cJSON_AddStringToObject(root, "hint",
		"img_qa uses [model.text]; configure it to a vision-capable "
		"chat model. img_gen uses [model.image], so image generation "
		"can work while image QA fails.");
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json)
		MORPH_RETURN(-ENOMEM);
	rc = tool_result_take_text(result, json);
	if (rc < 0)
		free(json);
	return rc;
}

static int img_qa_exec(const char *args_json, struct tool_result *result,
		       void *user_data)
{
	struct img_qa_context *ctx = user_data;
	struct tool_context *tctx = ctx ? ctx->tctx : NULL;
	struct model *llm = ctx ? ctx->llm : NULL;
	int rc = 0;
	cJSON *root = NULL;
	struct arena *arena = NULL;
	morph_buf_t buf;
	struct tool_runtime_stream_sink stream;
	struct model_image_chat_options opts;
	char resolved_path[PATH_MAX];
	const char *prompt;
	const char *file_path;

	memset(&buf, 0, sizeof(buf));
	resolved_path[0] = '\0';

	if (!result)
		return -EINVAL;
	if (!llm || !llm->api_key[0]) {
		(void)tool_result_take_text(result,
			strdup("{\"error\":\"no multimodal LLM configured\"}"));
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}
	if (!llm->chat_with_image) {
		(void)tool_result_take_text(result,
			strdup("{\"error\":\"LLM does not support image input\"}"));
		return -ENOSYS;
	}

	root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_take_text(result,
			strdup("{\"error\":\"invalid JSON\"}"));
		return -EINVAL;
	}

	cJSON *fp = cJSON_GetObjectItem(root, "file_path");
	cJSON *pr = cJSON_GetObjectItem(root, "prompt");
	file_path = cJSON_IsString(fp) ? fp->valuestring : NULL;
	prompt = cJSON_IsString(pr) && pr->valuestring && pr->valuestring[0]
		? pr->valuestring : "Analyze this image.";
	opts.max_tokens = img_qa_int_arg(root, "max_tokens", 1024, 1, 4096,
					 result);
	if (opts.max_tokens < 0) {
		rc = opts.max_tokens;
		goto out;
	}
	opts.timeout_seconds = img_qa_int_arg(root, "timeout_seconds", 120,
					     5, 300, result);
	if (opts.timeout_seconds < 0) {
		rc = (int)opts.timeout_seconds;
		goto out;
	}
	opts.max_dim = img_qa_int_arg(root, "max_dim", 360, 128, 1024,
				      result);
	if (opts.max_dim < 0) {
		rc = opts.max_dim;
		goto out;
	}
	if (!file_path || !*file_path) {
		(void)tool_result_take_text(result, strdup(
			"{\"error\":\"missing 'file_path' parameter. "
			"Usage: img_qa({\\\"file_path\\\": \\\"img.png\\\", "
			"\\\"prompt\\\": \\\"What is in this image?\\\"})\"}"));
		rc = -EINVAL;
		goto out;
	}

	if (tctx) {
		rc = tool_context_authorize_path(tctx, TOOL_PATH_READ,
						 file_path, resolved_path,
						 sizeof(resolved_path));
		if (rc < 0) {
			if (rc == -ENOENT)
				(void)tool_result_take_text(result, strdup(
					"{\"error\":\"image file not found\"}"));
			else
				(void)tool_result_take_text(result, strdup(
					"{\"error\":\"read path outside workspace: permission denied\"}"));
			goto out;
		}
	} else {
		strncpy(resolved_path, file_path, sizeof(resolved_path) - 1);
		resolved_path[sizeof(resolved_path) - 1] = '\0';
	}

	arena = arena_create(128 * 1024);
	if (!arena) {
		rc = -ENOMEM;
		(void)tool_result_take_text(result,
			strdup("{\"error\":\"memory allocation failed\"}"));
		goto out;
	}
	rc = morph_buf_init(&buf, 8192);
	if (rc != 0) {
		(void)tool_result_take_text(result,
			strdup("{\"error\":\"buffer allocation failed\"}"));
		goto out;
	}

	stream.buf = &buf;
	stream.tool = "img_qa";
	stream.kind = "text";
	(void)tool_runtime_emit_stream("img_qa", "status",
				       "sending image question to model");
	rc = llm->chat_with_image(
		llm, arena,
		"You answer questions about the provided image. "
		"Use only visual evidence from the image. If the task asks for "
		"OCR, transcribe visible text and say when text is unclear.",
		prompt, resolved_path, &opts, tool_runtime_stream_to_buf_cb,
		&stream);
	if (rc < 0) {
		morph_buf_cleanup(&buf);
		(void)img_qa_take_call_error(result, llm);
		goto out;
	}

	(void)tool_result_take_text(result, morph_buf_detach(&buf));
	if (!result->text.data) {
		rc = -ENOMEM;
		(void)tool_result_take_text(result,
			strdup("{\"error\":\"buffer allocation failed\"}"));
	}

out:
	if (buf.data)
		morph_buf_cleanup(&buf);
	if (arena)
		arena_destroy(arena);
	if (root)
		cJSON_Delete(root);
	return rc;
}

int img_qa_init(struct tool_registry *reg, struct model *llm,
		struct tool_context *tctx)
{
	if (!reg)
		return -EINVAL;
	struct img_qa_context *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->llm = llm;
	ctx->tctx = tctx;
	int rc = tool_register(TOOL_ORIGIN_BUILTIN, reg, "img_qa",
		"Answer questions about an image using the multimodal LLM. "
		"Use this for image understanding, OCR, scene description, "
		"visual comparison, and content analysis. Before upload, the "
		"image is compressed within max_dim while preserving aspect "
		"ratio. Provide file_path and prompt. Optional max_tokens "
		"(default 1024), timeout_seconds (default 120), and max_dim "
		"(default 360).",
		"{\"type\":\"object\",\"properties\":{"
		"\"file_path\":{\"type\":\"string\","
		"\"description\":\"Path to the image file\"},"
		"\"prompt\":{\"type\":\"string\","
		"\"description\":\"Question or instruction for image analysis, "
		"such as OCR or scene description\"},"
		"\"max_tokens\":{\"type\":\"integer\","
		"\"description\":\"Maximum output tokens (default 1024)\"},"
		"\"timeout_seconds\":{\"type\":\"integer\","
		"\"description\":\"Hard request timeout in seconds (default 120)\"},"
		"\"max_dim\":{\"type\":\"integer\","
		"\"description\":\"Maximum image side sent to model (default 360)\"}},"
		"\"required\":[\"file_path\"]}",
		img_qa_exec, ctx, img_qa_context_destroy);
	if (rc < 0)
		free(ctx);
	else
		tool_set_timeout(reg, "img_qa", 120);
	return rc;
}
