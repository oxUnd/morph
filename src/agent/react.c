#include "react.h"
#include "tokenizer.h"
#include "models/llm.h"
#include "util/log.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

const char *react_step_type_name(enum react_step_type type)
{
	switch (type) {
	case REACT_STEP_THOUGHT:	return "Thought";
	case REACT_STEP_ACTION:		return "Action";
	case REACT_STEP_OBSERVATION:	return "Observation";
	case REACT_STEP_FINAL:		return "Final";
	default:			return "Unknown";
	}
}

const char *react_state_name(enum react_state state)
{
	switch (state) {
	case REACT_STATE_INIT:		return "INIT";
	case REACT_STATE_THINKING:	return "THINKING";
	case REACT_STATE_ACTING:	return "ACTING";
	case REACT_STATE_OBSERVING:	return "OBSERVING";
	case REACT_STATE_FINAL:		return "FINAL";
	case REACT_STATE_DONE:		return "DONE";
	case REACT_STATE_ABORT:		return "ABORT";
	case REACT_STATE_TOOL_FAIL:	return "TOOL_FAIL";
	default:			return "Unknown";
	}
}

struct react_context *react_context_create(struct tool_registry *tools,
					   struct tokenizer *tok,
					   struct compress_config *cfg)
{
	struct react_context *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;
	ctx->tools = tools;
	ctx->tokenizer = tok;
	ctx->max_iterations = 10;
	ctx->step_timeout_seconds = 60;
	ctx->tool_max_retries = 3;
	ctx->state = REACT_STATE_INIT;
	if (cfg)
		ctx->compress = *cfg;
	return ctx;
}

void react_context_destroy(struct react_context *ctx)
{
	if (!ctx)
		return;
	react_reset(ctx);
	free(ctx->final_answer);
	free(ctx);
}

void react_reset(struct react_context *ctx)
{
	if (!ctx)
		return;
	struct react_step *cur = ctx->steps;
	while (cur) {
		struct react_step *next = cur->next;
		react_step_destroy(cur);
		cur = next;
	}
	ctx->steps = NULL;
	ctx->step_count = 0;
	ctx->state = REACT_STATE_INIT;
	free(ctx->final_answer);
	ctx->final_answer = NULL;
	if (ctx->messages) {
		msg_list_destroy(ctx->messages);
		ctx->messages = NULL;
	}
}

struct react_step *react_step_create(enum react_step_type type,
				     const char *content,
				     const char *tool_name,
				     const char *tool_args)
{
	struct react_step *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->type = type;
	s->content = content ? strdup(content) : NULL;
	s->tool_name = tool_name ? strdup(tool_name) : NULL;
	s->tool_args = tool_args ? strdup(tool_args) : NULL;
	return s;
}

void react_step_destroy(struct react_step *step)
{
	if (!step)
		return;
	free(step->content);
	free(step->tool_name);
	free(step->tool_args);
	free(step);
}

static void add_step(struct react_context *ctx, struct react_step *step)
{
	if (!ctx->steps) {
		ctx->steps = step;
	} else {
		struct react_step *cur = ctx->steps;
		while (cur->next)
			cur = cur->next;
		cur->next = step;
	}
	ctx->step_count++;
}

static int parse_action(const char *text, char *tool_name, size_t tn_size,
			char *tool_args, size_t ta_size)
{
	if (!text)
		return -1;
	while (*text && isspace((unsigned char)*text))
		text++;
	size_t i = 0;
	while (*text && *text != '(' && !isspace((unsigned char)*text) && i < tn_size - 1)
		tool_name[i++] = *text++;
	tool_name[i] = '\0';
	if (*text != '(')
		return -1;
	text++;
	char *end = strchr(text, ')');
	if (!end)
		return -1;
	size_t arg_len = (size_t)(end - text);
	if (arg_len >= ta_size)
		arg_len = ta_size - 1;
	memcpy(tool_args, text, arg_len);
	tool_args[arg_len] = '\0';
	return 0;
}

static char *extract_after_prefix(const char *response, const char *prefix)
{
	if (!response || !prefix)
		return NULL;
	const char *p = strcasestr(response, prefix);
	if (!p)
		return NULL;
	p += strlen(prefix);
	while (*p && isspace((unsigned char)*p))
		p++;
	if (!*p)
		return strdup("");
	return strdup(p);
}

static int build_prompt(struct react_context *ctx, const char *user_input,
			char **out_prompt)
{
	size_t cap = 16384;
	size_t len = 0;
	char *buf = malloc(cap);
	if (!buf)
		return -ENOMEM;

	len += snprintf(buf + len, cap - len,
		"You are a multi-modal content creation assistant.\n");
	if (ctx->tools && ctx->tools->count > 0) {
		len += snprintf(buf + len, cap - len,
			"Available tools:\n");
		for (int i = 0; i < ctx->tools->count; i++) {
			len += snprintf(buf + len, cap - len, "- %s: %s\n",
				ctx->tools->entries[i].desc.name,
				ctx->tools->entries[i].desc.desc);
		}
	}
	len += snprintf(buf + len, cap - len,
		"\nOutput format (strict):\n"
		"Thought: <your reasoning>\n"
		"Action: <tool_name>(<json_args>)\n"
		"\nAfter each Action you will receive:\n"
		"Observation: <tool result or error>\n"
		"\nWhen done, output exactly:\n"
		"Final: <your answer>\n"
		"\nConstraints:\n"
		"- One Thought + one Action per turn.\n"
		"- If no tool is needed, go straight to Final.\n"
		"- Maximum %d iterations.\n\n",
		ctx->max_iterations);

	struct react_step *step = ctx->steps;
	while (step) {
		switch (step->type) {
		case REACT_STEP_THOUGHT:
			len += snprintf(buf + len, cap - len,
				"Thought: %s\n", step->content ? step->content : "");
			break;
		case REACT_STEP_ACTION:
			len += snprintf(buf + len, cap - len,
				"Action: %s(%s)\n",
				step->tool_name ? step->tool_name : "",
				step->tool_args ? step->tool_args : "");
			break;
		case REACT_STEP_OBSERVATION:
			len += snprintf(buf + len, cap - len,
				"Observation: %s\n",
				step->content ? step->content : "");
			break;
		default:
			break;
		}
		if (len + 1024 > cap) {
			cap *= 2;
			char *new_buf = realloc(buf, cap);
			if (!new_buf) { free(buf); return -ENOMEM; }
			buf = new_buf;
		}
		step = step->next;
	}

	len += snprintf(buf + len, cap - len, "\nUser: %s\n", user_input);
	*out_prompt = buf;
	return 0;
}

/* Store the accumulated LLM response for parsing */
struct react_stream_data {
	char *response;
	size_t len;
	size_t cap;
	react_output_cb user_cb;
	void *user_data;
};

static int react_stream_cb(const char *token, void *user_data)
{
	struct react_stream_data *sd = user_data;
	size_t tlen = strlen(token);
	if (sd->len + tlen + 1 >= sd->cap) {
		sd->cap = (sd->len + tlen + 1) * 2;
		char *new_resp = realloc(sd->response, sd->cap);
		if (!new_resp)
			return -ENOMEM;
		sd->response = new_resp;
	}
	memcpy(sd->response + sd->len, token, tlen);
	sd->len += tlen;
	sd->response[sd->len] = '\0';

	if (sd->user_cb)
		sd->user_cb(REACT_STEP_THOUGHT, token, sd->user_data);
	return 0;
}

int react_run(struct react_context *ctx, const char *user_input,
	      react_output_cb cb, void *user_data)
{
	if (!ctx || !user_input)
		return -EINVAL;
	react_reset(ctx);
	ctx->state = REACT_STATE_THINKING;

	struct message_list *msg = msg_list_create("user", user_input,
						  tokenizer_count(ctx->tokenizer, user_input));
	msg_list_append(&ctx->messages, msg);

	int has_tools = ctx->tools && ctx->tools->count > 0;

	for (int iteration = 0; iteration < ctx->max_iterations; iteration++) {
		ctx->state = REACT_STATE_THINKING;

		char *prompt = NULL;
		int rc = build_prompt(ctx, user_input, &prompt);
		if (rc < 0) {
			log_err("react_run: failed to build prompt");
			ctx->state = REACT_STATE_ABORT;
			return rc;
		}

		struct model *llm = (struct model *)ctx->llm_model;

		/* If no LLM model available, output prompt as thought + final (fallback) */
		if (!llm || !llm->api_key[0]) {
			struct react_step *thought = react_step_create(
				REACT_STEP_THOUGHT, "Processing user input...", NULL, NULL);
			add_step(ctx, thought);
			if (cb)
				cb(REACT_STEP_THOUGHT, "Processing user input...", user_data);

			free(ctx->final_answer);
			ctx->final_answer = strdup(user_input);

			struct react_step *final_step = react_step_create(
				REACT_STEP_FINAL, user_input, NULL, NULL);
			add_step(ctx, final_step);
			if (cb)
				cb(REACT_STEP_FINAL, user_input, user_data);

			ctx->state = REACT_STATE_DONE;
			free(prompt);
			break;
		}

		/* Call LLM */
		const char *msgs[] = { prompt };
		struct react_stream_data sd = {
			.response = malloc(8192),
			.len = 0,
			.cap = 8192,
			.user_cb = cb,
			.user_data = user_data,
		};
		if (!sd.response) {
			free(prompt);
			ctx->state = REACT_STATE_ABORT;
			return -ENOMEM;
		}
		sd.response[0] = '\0';

		if (cb)
			cb(REACT_STEP_THOUGHT, "", user_data);

int status = llm->chat(llm, NULL, msgs, 1,
					  react_stream_cb, &sd);

		if (status < 0) {
			log_err("react_run: LLM call failed: %d", status);
			struct react_step *err = react_step_create(
				REACT_STEP_OBSERVATION, "LLM call failed", NULL, NULL);
			add_step(ctx, err);
			free(sd.response);
			free(prompt);
			ctx->state = REACT_STATE_ABORT;
			return status;
		}

		free(prompt);

		/* Parse LLM response for Thought/Action/Final patterns */
		char *final_text = extract_after_prefix(sd.response, "Final:");
		char *action_text = extract_after_prefix(sd.response, "Action:");
		char *thought_text = extract_after_prefix(sd.response, "Thought:");

		if (thought_text) {
			char *newline = strchr(thought_text, '\n');
			if (newline) *newline = '\0';
			struct react_step *thought = react_step_create(
				REACT_STEP_THOUGHT, thought_text, NULL, NULL);
			add_step(ctx, thought);
			free(thought_text);
		}

		if (final_text) {
			/* LLM produced Final: answer */
			struct react_step *final_step = react_step_create(
				REACT_STEP_FINAL, final_text, NULL, NULL);
			add_step(ctx, final_step);
			free(ctx->final_answer);
			ctx->final_answer = final_text;
			ctx->state = REACT_STATE_DONE;
			free(action_text);
			free(sd.response);
			break;
		} else if (action_text && has_tools) {
			/* LLM wants to call a tool */
			ctx->state = REACT_STATE_ACTING;
			char tool_name[64] = {0};
			char tool_args[1024] = {0};
			if (parse_action(action_text, tool_name, sizeof(tool_name),
					tool_args, sizeof(tool_args)) == 0) {
				struct react_step *action = react_step_create(
					REACT_STEP_ACTION, action_text, tool_name, tool_args);
				add_step(ctx, action);
				if (cb)
					cb(REACT_STEP_ACTION, action_text, user_data);

				/* Execute the tool */
				ctx->state = REACT_STATE_OBSERVING;
				char *result = NULL;
				int tool_rc = tool_exec(ctx->tools, tool_name,
							tool_args, &result);
				char obs_buf[4096];
				if (tool_rc < 0) {
					snprintf(obs_buf, sizeof(obs_buf),
						"tool error: %s (code %d)",
						result ? result : "unknown error", tool_rc);
				} else {
					snprintf(obs_buf, sizeof(obs_buf), "%s",
						result ? result : "(no output)");
				}
				free(result);
				free(action_text);

				struct react_step *obs = react_step_create(
					REACT_STEP_OBSERVATION, obs_buf, NULL, NULL);
				add_step(ctx, obs);
				if (cb)
					cb(REACT_STEP_OBSERVATION, obs_buf, user_data);
			} else {
				free(action_text);
			}
			/* Continue loop - next iteration will include observation */
		} else {
			/* No Final or Action found - treat entire response as final */
			struct react_step *final_step = react_step_create(
				REACT_STEP_FINAL, sd.response, NULL, NULL);
			add_step(ctx, final_step);
			free(ctx->final_answer);
			ctx->final_answer = strdup(sd.response);
			ctx->state = REACT_STATE_DONE;
			free(action_text);
			free(sd.response);
			break;
		}

		free(sd.response);
	}

	if (ctx->state == REACT_STATE_ABORT)
		return -1;
	return 0;
}