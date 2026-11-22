#ifndef MORPH_MODELS_LLM_CONTENT_H
#define MORPH_MODELS_LLM_CONTENT_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

cJSON *llm_content_create(const char *prefix, const cJSON *parts, int responses);
/* Move media unsupported in a tool-result position to a later user message. */
int llm_content_tool_split(cJSON *parts, cJSON *attachments, int responses);
int llm_content_attachments_append(cJSON *messages, cJSON *attachments);

#ifdef __cplusplus
}
#endif

#endif
