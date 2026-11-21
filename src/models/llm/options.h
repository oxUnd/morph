#ifndef MORPH_LLM_OPTIONS_H
#define MORPH_LLM_OPTIONS_H

#include <stddef.h>
#include <string.h>

/* Shared by configuration validation and both request codecs. */
static inline int llm_option_key_is_reserved(const char *key)
{
	static const char *const reserved[] = {
		"model", "messages", "tools", "tool_choice", "stream", "stream_options",
		"max_tokens", "input", "instructions", "parallel_tool_calls", "max_output_tokens"
	};

	if (!key)
		return 0;

	for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
		if (!strcmp(key, reserved[i]))
			return 1;
	}

	return 0;
}

#endif
