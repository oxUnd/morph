#include "react.h"
#include "tokenizer.h"
#include "compress.h"
#include "models/llm.h"
#include "util/log.h"
#include "util/arena.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>

volatile sig_atomic_t react_sigint_flag = 0;

static int case_ncmp(const char *s1, const char *s2, size_t n)
{
	while (n-- > 0) {
		int c1 = tolower((unsigned char)*s1++);
		int c2 = tolower((unsigned char)*s2++);
		if (c1 != c2)
			return c1 - c2;
		if (c1 == '\0')
			return 0;
	}
	return 0;
}

static const char *find_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !*needle)
		return haystack;
	size_t needle_len = strlen(needle);
	while (*haystack) {
		if (case_ncmp(haystack, needle, needle_len) == 0)
			return haystack;
		haystack++;
	}
	return NULL;
}

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
	ctx->cancelled = 0;
	ctx->arena = arena_create(0);
	if (cfg)
		ctx->compress = *cfg;
	return ctx;
}

void react_context_destroy(struct react_context *ctx)
{
	if (!ctx)
		return;
	react_reset(ctx);
	if (ctx->messages) {
		msg_list_destroy(ctx->messages);
		ctx->messages = NULL;
	}
	free(ctx->final_answer);
	free(ctx->system_prompt);
	arena_destroy(ctx->arena);
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
	ctx->tool_fail_name[0] = '\0';
	ctx->tool_fail_args[0] = '\0';
	ctx->tool_fail_count = 0;
	ctx->cancelled = 0;
}

void react_cancel(struct react_context *ctx)
{
	if (ctx)
		ctx->cancelled = 1;
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
	if (!step)
		return;
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
	while (*text && isspace((unsigned char)*text))
		text++;
	if (*text != '(')
		return -1;
	text++;

	/* Walk char-by-char, tracking string and bracket nesting so that we
	 * correctly handle JSON args like {"path":"a)b","n":1}. */
	int paren = 1;     /* we already consumed the opening '(' */
	int brace = 0;
	int bracket = 0;
	int in_str = 0;
	int escape = 0;
	const char *p = text;
	const char *close = NULL;
	for (; *p; p++) {
		char c = *p;
		if (in_str) {
			if (escape) {
				escape = 0;
			} else if (c == '\\') {
				escape = 1;
			} else if (c == '"') {
				in_str = 0;
			}
			continue;
		}
		if (c == '"') {
			in_str = 1;
		} else if (c == '{') {
			brace++;
		} else if (c == '}') {
			if (brace > 0)
				brace--;
		} else if (c == '[') {
			bracket++;
		} else if (c == ']') {
			if (bracket > 0)
				bracket--;
		} else if (c == '(') {
			paren++;
		} else if (c == ')') {
			paren--;
			if (paren == 0 && brace == 0 && bracket == 0) {
				close = p;
				break;
			}
		}
	}
	if (!close)
		return -1;
	size_t arg_len = (size_t)(close - text);
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
	const char *p = find_ci(response, prefix);
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
	if (ctx->system_prompt) {
		len += snprintf(buf + len, cap - len, "%s\n", ctx->system_prompt);
	}
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
		"- Action MUST be a tool listed above. If no tool is needed, go straight to Final.\n"
		"- If a tool fails twice with the same args, change strategy or finalize.\n"
		"- Maximum %d iterations.\n\n"
		"\nConversation history:\n",
		ctx->max_iterations);

	struct message_list *hist = ctx->messages;
	int hist_count = msg_list_count(ctx->messages);
	int cur_idx = 0;
	while (hist && cur_idx < hist_count - 1) {
		const char *role_label = (strcmp(hist->role, "assistant") == 0) ? "Assistant" : "User";
		len += snprintf(buf + len, cap - len, "%s: %s\n", role_label,
				hist->content ? hist->content : "");
		if (len + 1024 > cap) {
			size_t new_cap = cap * 2;
			while (new_cap < len + 1024)
				new_cap *= 2;
			char *new_buf = realloc(buf, new_cap);
			if (!new_buf) { free(buf); return -ENOMEM; }
			cap = new_cap;
			buf = new_buf;
		}
		hist = hist->next;
		cur_idx++;
	}

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
			size_t new_cap = cap * 2;
			while (new_cap < len + 1024)
				new_cap *= 2;
			char *new_buf = realloc(buf, new_cap);
			if (!new_buf) { free(buf); return -ENOMEM; }
			cap = new_cap;
			buf = new_buf;
		}
		step = step->next;
	}

	len += snprintf(buf + len, cap - len, "\nUser: %s\n", user_input);
	*out_prompt = buf;
	return 0;
}

struct react_stream_data {
	char *response;
	size_t len;
	size_t cap;
	react_output_cb user_cb;
	void *user_data;
	volatile sig_atomic_t *cancelled;
};

static int react_stream_cb(const char *token, void *user_data)
{
	struct react_stream_data *sd = user_data;
	if (react_sigint_flag) {
		if (sd->cancelled)
			*sd->cancelled = 1;
		react_sigint_flag = 0;
	}
	if (sd->cancelled && *sd->cancelled)
		return -EINTR;
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
	arena_reset(ctx->arena);
	ctx->state = REACT_STATE_THINKING;

	struct message_list *msg = msg_list_create("user", user_input,
						  tokenizer_count(ctx->tokenizer, user_input));
	msg_list_append(&ctx->messages, msg);

	int has_tools = ctx->tools && ctx->tools->count > 0;

	for (int iteration = 0; iteration < ctx->max_iterations; iteration++) {
		if (react_sigint_flag) {
			ctx->cancelled = 1;
			react_sigint_flag = 0;
		}
		if (ctx->cancelled) {
			log_info("react_run: cancelled by user at iteration %d", iteration);
			ctx->state = REACT_STATE_ABORT;
			break;
		}

		ctx->state = REACT_STATE_THINKING;

		if (context_needs_compress(ctx->messages, ctx->tokenizer,
					   &ctx->compress)) {
			struct compress_result cr = {0};
			compress_sliding_window(&ctx->messages,
						ctx->compress.max_history_rounds,
						&cr);
			log_info("auto-compress: removed %d messages (%d -> %d tokens)",
				 cr.messages_removed, cr.original_tokens,
				 cr.compressed_tokens);
			free(cr.summary);
			key_info_free(cr.preserved);
		}

		char *prompt = NULL;
		int rc = build_prompt(ctx, user_input, &prompt);
		if (rc < 0) {
			log_err("react_run: failed to build prompt");
			ctx->state = REACT_STATE_ABORT;
			return rc;
		}

		struct model *llm = (struct model *)ctx->llm_model;

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

		const char *msgs[] = { prompt };
		struct react_stream_data sd = {
			.response = malloc(8192),
			.len = 0,
			.cap = 8192,
			.user_cb = cb,
			.user_data = user_data,
			.cancelled = &ctx->cancelled,
		};
		if (!sd.response) {
			free(prompt);
			ctx->state = REACT_STATE_ABORT;
			return -ENOMEM;
		}
		sd.response[0] = '\0';

		if (cb)
			cb(REACT_STEP_THOUGHT, "", user_data);

		time_t llm_start = time(NULL);
		int status = llm->chat(llm, NULL, msgs, 1, react_stream_cb, &sd);
		time_t llm_end = time(NULL);

		free(prompt);

		if (react_sigint_flag) {
			ctx->cancelled = 1;
			react_sigint_flag = 0;
		}
		if (ctx->cancelled) {
			log_info("react_run: cancelled during LLM call");
			struct react_step *obs = react_step_create(
				REACT_STEP_OBSERVATION,
				"LLM call interrupted by user", NULL, NULL);
			add_step(ctx, obs);
			if (sd.response[0]) {
				free(ctx->final_answer);
				ctx->final_answer = strdup(sd.response);
			}
			ctx->state = REACT_STATE_ABORT;
			free(sd.response);
			break;
		}

		if (status < 0) {
			log_err("react_run: LLM call failed: %d", status);
			struct react_step *err = react_step_create(
				REACT_STEP_OBSERVATION, "LLM call failed", NULL, NULL);
			add_step(ctx, err);
			free(sd.response);
			ctx->state = REACT_STATE_ABORT;
			return status;
		}

		if (ctx->step_timeout_seconds > 0 &&
		    (llm_end - llm_start) >= ctx->step_timeout_seconds) {
			log_warn("react_run: LLM call exceeded step timeout (%lds >= %ds)",
				 (long)(llm_end - llm_start),
				 ctx->step_timeout_seconds);
			char timeout_msg[256];
			snprintf(timeout_msg, sizeof(timeout_msg),
				 "LLM call timed out (took %lds, limit %ds)",
				 (long)(llm_end - llm_start),
				 ctx->step_timeout_seconds);
			struct react_step *obs = react_step_create(
				REACT_STEP_OBSERVATION, timeout_msg, NULL, NULL);
			add_step(ctx, obs);
			if (cb)
				cb(REACT_STEP_OBSERVATION, timeout_msg, user_data);
			if (sd.response[0]) {
				char *final_text = extract_after_prefix(sd.response, "Final:");
				if (final_text) {
					free(ctx->final_answer);
					ctx->final_answer = final_text;
				} else {
					free(ctx->final_answer);
					ctx->final_answer = strdup(sd.response);
				}
			}
			free(sd.response);
			ctx->state = REACT_STATE_DONE;
			break;
		}

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
			struct react_step *final_step = react_step_create(
				REACT_STEP_FINAL, final_text, NULL, NULL);
			add_step(ctx, final_step);
			free(ctx->final_answer);
			ctx->final_answer = strdup(final_text);
			ctx->state = REACT_STATE_DONE;
			if (cb)
				cb(REACT_STEP_FINAL, final_text, user_data);
			free(final_text);
			free(action_text);
			free(sd.response);
			break;
		} else if (action_text && has_tools) {
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

				if (react_sigint_flag) {
					ctx->cancelled = 1;
					react_sigint_flag = 0;
				}
				if (ctx->cancelled) {
					struct react_step *obs = react_step_create(
						REACT_STEP_OBSERVATION,
						"Tool execution cancelled by user", NULL, NULL);
					add_step(ctx, obs);
					ctx->state = REACT_STATE_ABORT;
					free(action_text);
					free(sd.response);
					break;
				}

				ctx->state = REACT_STATE_OBSERVING;
				char *result = NULL;
				time_t tool_start = time(NULL);
				int tool_rc = tool_exec(ctx->tools, tool_name,
							tool_args, &result);
				time_t tool_end = time(NULL);

				if (ctx->step_timeout_seconds > 0 &&
				    (tool_end - tool_start) >= ctx->step_timeout_seconds) {
					log_warn("react_run: tool '%s' exceeded step timeout (%lds >= %ds)",
						 tool_name, (long)(tool_end - tool_start),
						 ctx->step_timeout_seconds);
					char timeout_msg[512];
					snprintf(timeout_msg, sizeof(timeout_msg),
						"tool error: '%s' timed out (took %lds, limit %ds)",
						tool_name, (long)(tool_end - tool_start),
						ctx->step_timeout_seconds);
					struct react_step *obs = react_step_create(
						REACT_STEP_OBSERVATION, timeout_msg, NULL, NULL);
					add_step(ctx, obs);
					if (cb)
						cb(REACT_STEP_OBSERVATION, timeout_msg, user_data);
					free(result);
					free(action_text);
					free(sd.response);
					continue;
				}

				char obs_buf[4096];
				if (tool_rc < 0) {
					ctx->state = REACT_STATE_TOOL_FAIL;
					snprintf(obs_buf, sizeof(obs_buf),
						"tool error: %s (code %d)",
						result ? result : "unknown error", tool_rc);
					if (strcmp(tool_name, ctx->tool_fail_name) == 0 &&
					    strcmp(tool_args, ctx->tool_fail_args) == 0) {
						/* Same tool + same args failed again. */
						ctx->tool_fail_count++;
					} else {
						strncpy(ctx->tool_fail_name, tool_name,
							sizeof(ctx->tool_fail_name) - 1);
						ctx->tool_fail_name[sizeof(ctx->tool_fail_name) - 1] = '\0';
						strncpy(ctx->tool_fail_args, tool_args,
							sizeof(ctx->tool_fail_args) - 1);
						ctx->tool_fail_args[sizeof(ctx->tool_fail_args) - 1] = '\0';
						ctx->tool_fail_count = 1;
					}
				} else {
					snprintf(obs_buf, sizeof(obs_buf), "%s",
						result ? result : "(no output)");
					ctx->tool_fail_name[0] = '\0';
					ctx->tool_fail_args[0] = '\0';
					ctx->tool_fail_count = 0;
				}
				free(result);
				free(action_text);

				if (ctx->tool_fail_count >= ctx->tool_max_retries) {
					log_warn("react_run: tool '%s' failed %d times consecutively, forcing Final",
						 ctx->tool_fail_name, ctx->tool_fail_count);
					char fail_msg[256];
					snprintf(fail_msg, sizeof(fail_msg),
						"Tool '%s' repeatedly failed. Please try a different approach.",
						ctx->tool_fail_name);
					struct react_step *final_step = react_step_create(
						REACT_STEP_FINAL, fail_msg, NULL, NULL);
					add_step(ctx, final_step);
					free(ctx->final_answer);
					ctx->final_answer = strdup(fail_msg);
					ctx->state = REACT_STATE_DONE;
					if (cb)
						cb(REACT_STEP_FINAL, fail_msg, user_data);
					free(sd.response);
					ctx->tool_fail_name[0] = '\0';
					ctx->tool_fail_args[0] = '\0';
					ctx->tool_fail_count = 0;
					break;
				}

				struct react_step *obs = react_step_create(
					REACT_STEP_OBSERVATION, obs_buf, NULL, NULL);
				add_step(ctx, obs);
				if (cb)
					cb(REACT_STEP_OBSERVATION, obs_buf, user_data);
			} else {
				struct react_step *obs = react_step_create(
					REACT_STEP_OBSERVATION,
					"tool error: invalid action format — expected Action: tool_name(args)",
					NULL, NULL);
				add_step(ctx, obs);
				if (cb)
					cb(REACT_STEP_OBSERVATION,
					   "tool error: invalid action format — expected Action: tool_name(args)",
					   user_data);
				free(action_text);
			}
		} else {
			struct react_step *final_step = react_step_create(
				REACT_STEP_FINAL, sd.response, NULL, NULL);
			add_step(ctx, final_step);
			free(ctx->final_answer);
			ctx->final_answer = strdup(sd.response);
			ctx->state = REACT_STATE_DONE;
			if (cb)
				cb(REACT_STEP_FINAL, sd.response, user_data);
			free(action_text);
			free(sd.response);
			break;
		}

		free(sd.response);
	}

	if (ctx->state != REACT_STATE_DONE && ctx->state != REACT_STATE_ABORT) {
		log_warn("react_run: max iterations (%d) reached, aborting", ctx->max_iterations);
		if (!ctx->final_answer) {
			struct react_step *last_obs = NULL;
			struct react_step *cur = ctx->steps;
			while (cur) {
				if (cur->type == REACT_STEP_OBSERVATION)
					last_obs = cur;
				cur = cur->next;
			}
			if (last_obs && last_obs->content) {
				ctx->final_answer = strdup(last_obs->content);
			} else {
				ctx->final_answer = strdup("Maximum iterations reached. No final answer produced.");
			}
		}
		ctx->state = REACT_STATE_ABORT;
	}

	if (ctx->state == REACT_STATE_DONE && ctx->steps) {
		const char *answer = ctx->final_answer ? ctx->final_answer : "(no answer)";
		struct message_list *asst = msg_list_create("assistant",
			answer,
			tokenizer_count(ctx->tokenizer, answer));
		msg_list_append(&ctx->messages, asst);
	}

	if (ctx->state == REACT_STATE_ABORT)
		return -1;
	return 0;
}