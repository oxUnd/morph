#include "img_qa.h"
#include "agent/tool_runtime.h"
#include "models/llm.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "util/cleanup.h"
#include "util/json.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/image_util.h"
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
		(void)tool_result_error_format(result, "tool_failed", "'%s' must be an integer",
					       name);
		return -EINVAL;
	}
	value = item->valueint;
	if (item->valuedouble != (double)value || value < min || value > max) {
		(void)tool_result_error_format(result, "tool_failed",
					       "'%s' must be between %d and %d", name, min, max);
		return -EINVAL;
	}
	return value;
}

static int img_qa_report_call_error(struct tool_result *result, struct model *llm)
{
	if (!result)
		return -EINVAL;
	cJSON *root MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateObject();
	if (!root || !cJSON_AddStringToObject(root, "error", "image QA LLM call failed"))
		return -ENOMEM;
	if (llm && llm->last_error[0] && !cJSON_AddStringToObject(root, "detail", llm->last_error))
		return -ENOMEM;
	if (!cJSON_AddStringToObject(root, "hint",
				     "img_qa uses [model.vision]; configure it to a vision-capable "
				     "chat model. img_gen uses [model.image], so image generation "
				     "can work while image QA fails."))
		return -ENOMEM;
	char *json MORPH_CLEANUP(morph_json_text_cleanup) = cJSON_PrintUnformatted(root);
	if (!json)
		return -ENOMEM;
	return tool_result_success_json_text_copy(result, json);
}

static int img_qa_exec(const char *args_json, struct tool_result *result,
		       void *user_data)
{
	struct img_qa_context local = {0};
	struct model request_model;

	if (user_data)
		local = *(struct img_qa_context *)user_data;
	local.llm = model_request_view_init(
		local.llm, result && result->runtime ? &result->runtime->http : NULL,
		&request_model);
	struct img_qa_context *ctx = user_data ? &local : NULL;
	struct tool_context *tctx = ctx ? ctx->tctx : NULL;
	struct model *llm = ctx ? ctx->llm : NULL;
	int rc = 0;
	cJSON *root MORPH_CLEANUP(morph_json_cleanup) = NULL;
	morph_buf_t buf MORPH_CLEANUP(morph_buf_cleanup) = {0};
	struct tool_runtime_stream_sink stream;
	struct model_image_chat_options opts;
	char resolved_path[PATH_MAX];
	const char *prompt;
	const char *file_path;

	memset(&buf, 0, sizeof(buf));
	resolved_path[0] = '\0';

	if (!result)
		return -EINVAL;
	if (!llm || !model_api_key_borrow(llm)[0]) {
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"no multimodal LLM configured\"}");
		return MORPH_ERR_NOT_CONFIGURED;
	}
	if (!llm->chat_with_image) {
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"LLM does not support image input\"}");
		return -ENOSYS;
	}

	root = cJSON_Parse(args_json);
	if (!root) {
		(void)tool_result_success_json_text_copy(result, "{\"error\":\"invalid JSON\"}");
		return -EINVAL;
	}

	cJSON *fp = cJSON_GetObjectItem(root, "file_path");
	cJSON *pr = cJSON_GetObjectItem(root, "prompt");
	file_path = cJSON_IsString(fp) ? fp->valuestring : NULL;
	prompt = cJSON_IsString(pr) && pr->valuestring && pr->valuestring[0]
		? pr->valuestring : "Analyze this image.";
	int output_budget = llm->max_tokens > 0 ? llm->max_tokens : 4096;
	opts.max_tokens = img_qa_int_arg(root, "max_tokens", output_budget, 1, output_budget,
					 result);
	if (opts.max_tokens < 0) {
		rc = opts.max_tokens;
		return rc;
	}
	opts.timeout_seconds = img_qa_int_arg(root, "timeout_seconds", 120,
					     5, 300, result);
	if (opts.timeout_seconds < 0) {
		rc = (int)opts.timeout_seconds;
		return rc;
	}
	const cJSON *detail = cJSON_GetObjectItemCaseSensitive(root, "detail");
	if (detail && (!cJSON_IsString(detail) ||
		       (strcmp(detail->valuestring, "high") != 0 &&
			strcmp(detail->valuestring, "original") != 0))) {
		(void)tool_result_error_copy(result, "invalid_detail",
			"'detail' must be 'high' or 'original'");
		return -EINVAL;
	}
	int image_budget = detail && strcmp(detail->valuestring, "original") == 0
		? MORPH_IMAGE_MAX_DIMENSION : 2048;
	opts.max_dim = img_qa_int_arg(root, "max_dim", image_budget, 128,
				      MORPH_IMAGE_MAX_DIMENSION,
				      result);
	if (opts.max_dim < 0) {
		rc = opts.max_dim;
		return rc;
	}
	if (!file_path || !*file_path) {
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"missing 'file_path' parameter. "
				"Usage: img_qa({\\\"file_path\\\": \\\"img.png\\\", "
				"\\\"prompt\\\": \\\"What is in this image?\\\"})\"}");
		rc = -EINVAL;
		return rc;
	}

	if (tctx) {
		rc = tool_context_authorize_path(tctx, TOOL_PATH_READ,
						 file_path, resolved_path,
						 sizeof(resolved_path));
		if (rc < 0) {
			if (rc == -ENOENT)
				(void)tool_result_success_json_text_copy(
					result, "{\"error\":\"image file not found\"}");
			else
				(void)tool_result_success_json_text_copy(
					result, "{\"error\":\"read path outside workspace: "
						"permission denied\"}");
			return rc;
		}
	} else {
		if (strlen(file_path) >= sizeof(resolved_path)) {
			(void)tool_result_error_copy(result, "path_too_long",
				"Image path exceeds the filesystem path limit");
			return -ENAMETOOLONG;
		}
		memcpy(resolved_path, file_path, strlen(file_path) + 1);
	}

	rc = morph_buf_init(&buf, 8192);
	if (rc != 0) {
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"buffer allocation failed\"}");
		return rc;
	}

	stream.runtime = result->runtime;
	stream.buf = &buf;
	stream.tool = "img_qa";
	stream.kind = "text";
	(void)tool_runtime_emit_stream(result->runtime, "img_qa", "status",
				       "sending image question to model");
	rc = llm->chat_with_image(llm,
				  "You answer questions about the provided image. "
				  "Use only visual evidence from the image. If the task asks for "
				  "OCR, transcribe visible text and say when text is unclear.",
				  prompt, resolved_path, &opts, tool_runtime_stream_to_buf_cb,
				  &stream);
	if (rc < 0) {
		morph_buf_cleanup(&buf);
		(void)img_qa_report_call_error(result, llm);
		return rc;
	}

	rc = tool_result_success_json_text_copy(result, morph_buf_cstr_borrow(&buf));
	if (rc < 0)
		(void)tool_result_success_json_text_copy(
			result, "{\"error\":\"buffer allocation failed\"}");

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
	struct tool_spec spec MORPH_CLEANUP(tool_spec_cleanup) = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.flags = TOOL_FLAG_READONLY | TOOL_FLAG_MEDIA,
		.name = "img_qa",
		.description = "Answer questions about an image using the multimodal LLM. "
			       "Use this for image understanding, OCR, scene description, "
			       "visual comparison, and content analysis. User messages reference "
			       "local images as [Image: <path>]; pass that path as file_path and "
			       "ask the relevant question in prompt. Use the returned analysis "
			       "instead of guessing from the filename or just repeating the path. "
			       "Before upload, the "
			       "image is prepared within max_dim while preserving its format "
			       "and aspect "
			       "ratio. High detail uses a maximum side of 2048 pixels; use "
			       "detail=original for OCR, small text, or fine visual detail. "
			       "Original detail preserves dimensions within shared image "
			       "safety limits. max_dim can override this policy. max_tokens "
			       "defaults to the configured vision model output budget. "
			       "timeout_seconds defaults to 120.",
		.input_schema =
			"{\"type\":\"object\",\"properties\":{"
			"\"file_path\":{\"type\":\"string\","
			"\"description\":\"Path to the image file\"},"
			"\"prompt\":{\"type\":\"string\","
			"\"description\":\"Question or instruction for image analysis, "
			"such as OCR or scene description\"},"
			"\"max_tokens\":{\"type\":\"integer\",\"minimum\":1,"
			"\"description\":\"Output token budget, at most the configured vision "
			"model budget; defaults to that budget\"},"
			"\"timeout_seconds\":{\"type\":\"integer\","
			"\"description\":\"Hard request timeout in seconds (default 120)\"},"
			"\"detail\":{\"type\":\"string\",\"enum\":[\"high\",\"original\"],"
			"\"default\":\"high\"},"
			"\"max_dim\":{\"type\":\"integer\",\"minimum\":128,\"maximum\":32768,"
			"\"description\":\"Maximum image side; defaults to 2048 for high detail "
			"and the original dimensions for original detail\"}},"
			"\"required\":[\"file_path\"]}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = img_qa_exec,
		.user_data = ctx,
		.user_data_destroy = img_qa_context_destroy};
	int rc = tool_register_take(reg, &spec);
	if (rc == 0)
		tool_set_timeout(reg, "img_qa", 120);
	return rc;
}
