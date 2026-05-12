#include "text_gen.h"
#include "util/log.h"
#include <stdlib.h>
#include <string.h>

static struct model *g_llm;

static int text_gen_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)user_data;
	if (!args_json || !result_json)
		return -EINVAL;

	const char *prompt = strstr(args_json, "\"prompt\"");
	if (!prompt) {
		*result_json = strdup("{\"error\":\"missing prompt parameter\"}");
		return -EINVAL;
	}

	const char *val_start = strchr(prompt + 9, '"');
	if (!val_start) {
		*result_json = strdup("{\"error\":\"invalid prompt value\"}");
		return -EINVAL;
	}
	val_start++;
	const char *val_end = strchr(val_start, '"');
	if (!val_end) {
		*result_json = strdup("{\"error\":\"invalid prompt value\"}");
		return -EINVAL;
	}

	size_t plen = (size_t)(val_end - val_start);
	char *prompt_str = malloc(plen + 1);
	if (!prompt_str)
		return -ENOMEM;
	memcpy(prompt_str, val_start, plen);
	prompt_str[plen] = '\0';

	if (!g_llm) {
		*result_json = strdup("{\"error\":\"no LLM model configured\"}");
		free(prompt_str);
		return -ENOSYS;
	}

	const char *msgs[] = { prompt_str };
	struct http_response resp = {0};
	int rc = http_post(g_llm->api_base, NULL, 0, "application/json", &resp);
	(void)rc;

	*result_json = strdup(prompt_str);
	free(prompt_str);
	return 0;
}

int text_gen_init(struct tool_registry *reg, struct model *llm)
{
	if (!reg)
		return -EINVAL;
	g_llm = llm;
	return tool_register(reg, "text_gen",
		"Generate text content based on a prompt",
		"{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"style\":{\"type\":\"string\"},\"length\":{\"type\":\"string\"}}}",
		text_gen_exec, NULL);
}