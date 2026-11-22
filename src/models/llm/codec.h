#ifndef MORPH_LLM_CODEC_H
#define MORPH_LLM_CODEC_H

#include "models/llm.h"
#include "adapter.h"
#include "stream.h"
#include <nb/sse.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

struct llm_codec {
	const char *endpoint;
	const char *input_key;
	const char *system_key;
	cJSON *(*input)(const struct model *model, const char *system_prompt,
			const struct chat_message *messages, int count);
	cJSON *(*tools)(const struct tool_desc *tools, int count);
	void (*configure)(const struct model *model, cJSON *request, int tool_count);
	nb_sse_callback event;
	int requires_terminal_event;
};

/* Immutable process-lifetime codec; callbacks borrow inputs and return owned JSON. */
const struct llm_codec *llm_codec_borrow(const struct model *model);
/* Borrow params and create an independent normalized schema. Invalid input or
 * allocation failure returns NULL without modifying or consuming params. */
cJSON *llm_codec_schema_copy(const cJSON *params);

int llm_codec_add_options(struct nb_log *logger, cJSON *root, const char *json);

int llm_codec_error_is_context(const char *code, const char *message);
/* Error payload and strings borrow their parsed JSON owner's lifetime. */
const cJSON *llm_codec_error_borrow(const cJSON *root);
const char *llm_codec_error_code_borrow(const cJSON *error);
const char *llm_codec_error_message_borrow(const cJSON *error);
enum model_error_kind llm_codec_error_kind(const cJSON *error);
int llm_codec_stream_error(struct llm_stream_ctx *ctx, const cJSON *error,
			   const char *detail);

#ifdef __cplusplus
}
#endif

#endif
