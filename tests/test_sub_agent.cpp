#include <gtest/gtest.h>
#include "agent/react.h"
#include "agent/tool.h"
#include "agent/tokenizer.h"
#include "agent/sub_agent.h"
#include "agent/context.h"
#include "models/llm.h"
#include "config.h"
#include "util/file.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

/* ---- mock LLM helpers (following test_react.cpp pattern) ---- */

struct mock_collect_data {
	char *buf;
	size_t len;
	size_t cap;
};

static int mock_collect_cb(const char *token, void *user_data)
{
	struct mock_collect_data *cd = (struct mock_collect_data *)user_data;
	size_t tlen = strlen(token);
	if (cd->len + tlen + 1 >= cd->cap) {
		cd->cap = (cd->len + tlen + 1) * 2;
		char *new_b = (char *)realloc(cd->buf, cd->cap);
		if (!new_b) return -ENOMEM;
		cd->buf = new_b;
	}
	memcpy(cd->buf + cd->len, token, tlen);
	cd->len += tlen;
	cd->buf[cd->len] = '\0';
	return 0;
}

struct mock_llm_data {
	const char *response;
	int call_count;
	int fail_after;
	int should_fail;
};

static char *strcasestr_local(const char *haystack, const char *needle)
{
	size_t nlen = strlen(needle);
	while (*haystack) {
		if (strncasecmp(haystack, needle, nlen) == 0)
			return (char *)haystack;
		haystack++;
	}
	return nullptr;
}

static int mock_llm_chat(struct model *self, struct arena *arena,
			  const char *system_prompt,
			  const char **messages, int n,
			  sse_callback cb, void *user_data)
{
	(void)arena; (void)system_prompt; (void)messages; (void)n;
	struct mock_llm_data *data = (struct mock_llm_data *)self->handle;
	data->call_count++;
	if (data->should_fail) return -EIO;
	if (data->fail_after > 0 && data->call_count > data->fail_after)
		return -EIO;
	if (cb && data->response) cb(data->response, user_data);
	return 200;
}

static int mock_chat_with_tools(struct model *self, struct arena *arena,
				const char *system_prompt,
				struct chat_message *messages, int msg_count,
				struct tool_desc *tools, int tool_count,
				struct chat_response *response,
				sse_callback thought_cb, void *thought_ud)
{
	(void)arena; (void)system_prompt; (void)messages;
	(void)msg_count; (void)tools; (void)tool_count;

	struct mock_collect_data cd = {nullptr, 0, 0};
	cd.buf = (char *)malloc(8192);
	cd.cap = 8192;
	cd.buf[0] = '\0';

	int status = self->chat(self, arena, nullptr, nullptr, 0,
				mock_collect_cb, &cd);
	if (status < 0) { free(cd.buf); return status; }

	memset(response, 0, sizeof(*response));

	char *action_pos = strcasestr_local(cd.buf, "Action:");
	if (action_pos && tool_count > 0) {
		if (action_pos > cd.buf) {
			size_t tlen = action_pos - cd.buf;
			char *thought = (char *)malloc(tlen + 1);
			memcpy(thought, cd.buf, tlen);
			thought[tlen] = '\0';
			while (tlen > 0 && isspace((unsigned char)thought[tlen-1]))
				thought[--tlen] = '\0';
			char *t = thought;
			if (strncasecmp(t, "Thought:", 8) == 0) { t += 8; while (*t == ' ') t++; }
			if (*t) { response->content = strdup(t); if (thought_cb) thought_cb(t, thought_ud); }
			free(thought);
		}
		const char *ap = action_pos + 7;
		while (*ap == ' ') ap++;
		char tool_name[64] = {0};
		int ni = 0;
		while (*ap && *ap != '(' && *ap != '\n' && ni < 63)
			tool_name[ni++] = *ap++;
		char *args = strdup("{}");
		if (*ap == '(') {
			ap++;
			const char *args_start = ap;
			int depth = 1;
			while (*ap && depth > 0) { if (*ap == '(') depth++; else if (*ap == ')') depth--; ap++; }
			size_t alen = (size_t)((ap - 1) - args_start);
			free(args);
			args = (char *)malloc(alen + 1);
			memcpy(args, args_start, alen);
			args[alen] = '\0';
		}
		response->tool_calls = (struct tool_call *)calloc(1, sizeof(*response->tool_calls));
		response->tool_call_count = 1;
		snprintf(response->tool_calls[0].id, sizeof(response->tool_calls[0].id), "call_mock_%d", 0);
		strncpy(response->tool_calls[0].name, tool_name, sizeof(response->tool_calls[0].name) - 1);
		response->tool_calls[0].arguments = args;
	} else {
		const char *content = cd.buf;
		char *final_pos = strcasestr_local(cd.buf, "Final:");
		if (final_pos) { final_pos += 6; while (*final_pos == ' ') final_pos++; content = final_pos; }
		response->content = strdup(content);
		if (thought_cb && response->content && *response->content)
			thought_cb(response->content, thought_ud);
	}
	free(cd.buf);
	return 200;
}

static void mock_llm_destroy(struct model *self)
{
	if (self && self->handle) { free(self->handle); self->handle = NULL; }
	free(self);
}

static struct model *create_mock_llm(const char *response)
{
	struct model *m = (struct model *)calloc(1, sizeof(*m));
	if (!m) return NULL;
	strncpy(m->provider, "mock", sizeof(m->provider) - 1);
	strncpy(m->model_id, "mock-model", sizeof(m->model_id) - 1);
	strncpy(m->api_key, "mock-key", sizeof(m->api_key) - 1);
	strncpy(m->api_base, "http://localhost:1", sizeof(m->api_base) - 1);
	m->context_limit = 128000;
	m->timeout_seconds = 60;
	m->chat = mock_llm_chat;
	m->chat_with_tools = mock_chat_with_tools;
	m->destroy = mock_llm_destroy;
	struct mock_llm_data *data = (struct mock_llm_data *)calloc(1, sizeof(*data));
	data->response = response;
	data->call_count = 0;
	data->fail_after = 0;
	data->should_fail = 0;
	m->handle = data;
	return m;
}

/* ---- mock tool ---- */

static int sa_test_tool_fn(const char *args_json, char **result_json, void *user_data)
{
	(void)args_json; (void)user_data;
	*result_json = strdup("{\"result\":\"sub_agent_test\"}");
	return 0;
}

/* ---- fixture ---- */

class SubAgentTest : public ::testing::Test {
protected:
	struct tool_registry tools;
	struct tokenizer *tok;
	struct compress_config cfg;
	struct model *llm;
	char config_path[256];
	void SetUp() override {
		tool_registry_init(&tools);
		tok = tokenizer_create("gpt-4o", 128000);
		cfg.max_context_tokens = 128000;
		cfg.max_history_rounds = 6;
		cfg.summarize_threshold_ratio = 0.8;
		cfg.compress_target_ratio = 0.5;
		llm = create_mock_llm("Final: sub-agent result");
		snprintf(config_path, sizeof(config_path),
			 "/tmp/ma_sa_test_%d.toml", getpid());
	}
	void TearDown() override {
		if (llm) model_destroy(llm);
		tokenizer_destroy(tok);
		std::remove(config_path);
	}
	void write_config(const char *toml) {
		file_write_all(config_path, toml, strlen(toml));
	}
};

/* ====================================================== */
/* 1. Runtime Lifecycle                                    */
/* ====================================================== */

TEST_F(SubAgentTest, RuntimeCreateDestroy) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	ASSERT_NE(rt, nullptr);
	EXPECT_EQ(rt->entry_count, 0);
	EXPECT_EQ(rt->depth, 0);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, RuntimeCreateNull) {
	/* create with NULL args is allowed — fields are simply NULL pointers */
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		NULL, NULL, NULL, NULL);
	ASSERT_NE(rt, nullptr);
	EXPECT_EQ(rt->parent_tools, nullptr);
	EXPECT_EQ(rt->default_llm, nullptr);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, RuntimeDestroyNull) {
	EXPECT_NO_FATAL_FAILURE(sub_agent_runtime_destroy(nullptr));
}

/* ====================================================== */
/* 2. Config Loading                                       */
/* ====================================================== */

TEST_F(SubAgentTest, LoadConfigEmpty) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	int rc = sub_agent_runtime_load_config(rt, &sa_cfg);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(rt->entry_count, 0);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, LoadConfigSingleEntry) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	struct config_sub_agent *e = &sa_cfg.entries[0];
	strncpy(e->name, "researcher", sizeof(e->name) - 1);
	strncpy(e->description, "Research agent", sizeof(e->description) - 1);
	e->max_iterations = 5;
	e->context_policy = SUB_AGENT_CTX_TASK_ONLY;
	e->merge_strategy = SUB_AGENT_MERGE_SYNTHESIZE;
	sa_cfg.count = 1;
	int rc = sub_agent_runtime_load_config(rt, &sa_cfg);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(rt->entry_count, 1);
	EXPECT_STREQ(rt->entries[0].cfg.name, "researcher");
	EXPECT_STREQ(rt->entries[0].cfg.description, "Research agent");
	EXPECT_EQ(rt->entries[0].cfg.max_iterations, 5);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, LoadConfigMultipleEntries) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	for (int i = 0; i < 3; i++) {
		snprintf(sa_cfg.entries[i].name, sizeof(sa_cfg.entries[i].name),
			 "agent_%d", i);
		snprintf(sa_cfg.entries[i].description,
			 sizeof(sa_cfg.entries[i].description),
			 "Agent number %d", i);
		sa_cfg.entries[i].max_iterations = 3 + i;
	}
	sa_cfg.count = 3;
	int rc = sub_agent_runtime_load_config(rt, &sa_cfg);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(rt->entry_count, 3);
	EXPECT_STREQ(rt->entries[2].cfg.name, "agent_2");
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, LoadConfigWithAllowedTools) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "minimal",
		sizeof(sa_cfg.entries[0].name) - 1);
	strncpy(sa_cfg.entries[0].allowed_tools[0], "text_gen",
		sizeof(sa_cfg.entries[0].allowed_tools[0]) - 1);
	strncpy(sa_cfg.entries[0].allowed_tools[1], "file_read",
		sizeof(sa_cfg.entries[0].allowed_tools[1]) - 1);
	sa_cfg.entries[0].allowed_tools_count = 2;
	sa_cfg.count = 1;
	int rc = sub_agent_runtime_load_config(rt, &sa_cfg);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(rt->entries[0].cfg.allowed_tools_count, 2);
	EXPECT_STREQ(rt->entries[0].cfg.allowed_tools[0], "text_gen");
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, LoadConfigWithDisabledTools) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "safe",
		sizeof(sa_cfg.entries[0].name) - 1);
	strncpy(sa_cfg.entries[0].disabled_tools[0], "bash_exec",
		sizeof(sa_cfg.entries[0].disabled_tools[0]) - 1);
	sa_cfg.entries[0].disabled_tools_count = 1;
	sa_cfg.count = 1;
	int rc = sub_agent_runtime_load_config(rt, &sa_cfg);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(rt->entries[0].cfg.disabled_tools_count, 1);
	EXPECT_STREQ(rt->entries[0].cfg.disabled_tools[0], "bash_exec");
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, LoadConfigNullArgs) {
	EXPECT_EQ(sub_agent_runtime_load_config(NULL, NULL), -EINVAL);
}

/* ====================================================== */
/* 3. Config TOML Parsing (end-to-end)                     */
/* ====================================================== */

TEST_F(SubAgentTest, ConfigTomlSubAgents) {
	const char *toml = R"(
[[agent.sub_agents]]
name = "researcher"
description = "Deep research agent"
max_iterations = 5
context_policy = "task_only"
merge_strategy = "synthesize"
)";
	write_config(toml);
	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(cfg.sub_agents.count, 1);
	EXPECT_STREQ(cfg.sub_agents.entries[0].name, "researcher");
	EXPECT_STREQ(cfg.sub_agents.entries[0].description, "Deep research agent");
	EXPECT_EQ(cfg.sub_agents.entries[0].max_iterations, 5);
	EXPECT_EQ(cfg.sub_agents.entries[0].context_policy, SUB_AGENT_CTX_TASK_ONLY);
	EXPECT_EQ(cfg.sub_agents.entries[0].merge_strategy, SUB_AGENT_MERGE_SYNTHESIZE);
}

TEST_F(SubAgentTest, ConfigTomlSubAgentsWithTools) {
	const char *toml = R"(
[[agent.sub_agents]]
name = "safe_agent"
description = "Safe agent"
allowed_tools = ["text_gen", "file_read"]
disabled_tools = ["bash_exec"]
)";
	write_config(toml);
	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(cfg.sub_agents.entries[0].allowed_tools_count, 2);
	EXPECT_STREQ(cfg.sub_agents.entries[0].allowed_tools[0], "text_gen");
	EXPECT_EQ(cfg.sub_agents.entries[0].disabled_tools_count, 1);
	EXPECT_STREQ(cfg.sub_agents.entries[0].disabled_tools[0], "bash_exec");
}

TEST_F(SubAgentTest, ConfigTomlDefaultPolicy) {
	const char *toml = R"(
[[agent.sub_agents]]
name = "default_policy"
description = "No policy specified"
)";
	write_config(toml);
	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(cfg.sub_agents.entries[0].context_policy,
		  SUB_AGENT_CTX_TASK_ONLY);
	EXPECT_EQ(cfg.sub_agents.entries[0].merge_strategy,
		  SUB_AGENT_MERGE_SYNTHESIZE);
}

TEST_F(SubAgentTest, ConfigTomlFullPolicy) {
	const char *toml = R"(
[[agent.sub_agents]]
name = "full_ctx"
description = "Full context agent"
context_policy = "full"
merge_strategy = "concat"
)";
	write_config(toml);
	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(cfg.sub_agents.entries[0].context_policy,
		  SUB_AGENT_CTX_FULL);
	EXPECT_EQ(cfg.sub_agents.entries[0].merge_strategy,
		  SUB_AGENT_MERGE_CONCAT);
}

TEST_F(SubAgentTest, ConfigTomlOutputSchema) {
	const char *toml = R"(
[[agent.sub_agents]]
name = "structured"
description = "Structured output"
output_schema = '{"type":"object","properties":{"x":{"type":"string"}}}'
)";
	write_config(toml);
	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(cfg.sub_agents.entries[0].output_schema, nullptr);
	EXPECT_TRUE(strstr(cfg.sub_agents.entries[0].output_schema,
			   "\"type\":\"object\"") != nullptr);
}

TEST_F(SubAgentTest, ConfigTomlMultipleEntries) {
	const char *toml = R"(
[[agent.sub_agents]]
name = "agent_a"
description = "Agent A"

[[agent.sub_agents]]
name = "agent_b"
description = "Agent B"
)";
	write_config(toml);
	struct config cfg;
	int rc = config_load(&cfg, config_path);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(cfg.sub_agents.count, 2);
	EXPECT_STREQ(cfg.sub_agents.entries[0].name, "agent_a");
	EXPECT_STREQ(cfg.sub_agents.entries[1].name, "agent_b");
}

/* ====================================================== */
/* 4. Entry Lookup                                        */
/* ====================================================== */

TEST_F(SubAgentTest, FindExisting) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "finder",
		sizeof(sa_cfg.entries[0].name) - 1);
	sa_cfg.count = 1;
	sub_agent_runtime_load_config(rt, &sa_cfg);
	struct sub_agent_entry *e = sub_agent_find(rt, "finder");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->cfg.name, "finder");
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, FindNonexistent) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	EXPECT_EQ(sub_agent_find(rt, "ghost"), nullptr);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, FindNullName) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	EXPECT_EQ(sub_agent_find(rt, nullptr), nullptr);
	sub_agent_runtime_destroy(rt);
}

/* ====================================================== */
/* 5. Tool Registry Building                              */
/* ====================================================== */

TEST_F(SubAgentTest, BuildRegistryNoAllowedTools) {
	tool_register(&tools, "text_gen", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	tool_register(&tools, "file_read", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	tool_register(&tools, "delegate", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	tool_register(&tools, "agent_status", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "worker",
		sizeof(sa_cfg.entries[0].name) - 1);
	sa_cfg.count = 1;
	sub_agent_runtime_load_config(rt, &sa_cfg);
	struct tool_registry *child = sub_agent_build_tool_registry(
		rt, &rt->entries[0]);
	ASSERT_NE(child, nullptr);
	EXPECT_GE(child->count, 2);
	EXPECT_NE(tool_lookup(child, "text_gen"), nullptr);
	EXPECT_NE(tool_lookup(child, "file_read"), nullptr);
	EXPECT_EQ(tool_lookup(child, "delegate"), nullptr);
	EXPECT_EQ(tool_lookup(child, "agent_status"), nullptr);
	tool_registry_cleanup(child);
	free(child);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, BuildRegistryWithAllowedTools) {
	tool_register(&tools, "text_gen", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	tool_register(&tools, "file_read", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	tool_register(&tools, "bash_exec", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "restricted",
		sizeof(sa_cfg.entries[0].name) - 1);
	strncpy(sa_cfg.entries[0].allowed_tools[0], "text_gen",
		sizeof(sa_cfg.entries[0].allowed_tools[0]) - 1);
	sa_cfg.entries[0].allowed_tools_count = 1;
	sa_cfg.count = 1;
	sub_agent_runtime_load_config(rt, &sa_cfg);
	struct tool_registry *child = sub_agent_build_tool_registry(
		rt, &rt->entries[0]);
	ASSERT_NE(child, nullptr);
	EXPECT_EQ(child->count, 1);
	EXPECT_NE(tool_lookup(child, "text_gen"), nullptr);
	EXPECT_EQ(tool_lookup(child, "file_read"), nullptr);
	tool_registry_cleanup(child);
	free(child);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, BuildRegistryWithDisabledTools) {
	tool_register(&tools, "text_gen", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	tool_register(&tools, "bash_exec", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "safe",
		sizeof(sa_cfg.entries[0].name) - 1);
	strncpy(sa_cfg.entries[0].disabled_tools[0], "bash_exec",
		sizeof(sa_cfg.entries[0].disabled_tools[0]) - 1);
	sa_cfg.entries[0].disabled_tools_count = 1;
	sa_cfg.count = 1;
	sub_agent_runtime_load_config(rt, &sa_cfg);
	struct tool_registry *child = sub_agent_build_tool_registry(
		rt, &rt->entries[0]);
	ASSERT_NE(child, nullptr);
	EXPECT_NE(tool_lookup(child, "bash_exec"), nullptr);
	EXPECT_TRUE(tool_is_disabled(child, "bash_exec"));
	tool_registry_cleanup(child);
	free(child);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, BuildRegistryNullEntry) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	EXPECT_EQ(sub_agent_build_tool_registry(rt, nullptr), nullptr);
	EXPECT_EQ(sub_agent_build_tool_registry(nullptr, nullptr), nullptr);
	sub_agent_runtime_destroy(rt);
}

/* ====================================================== */
/* 6. Context Creation                                    */
/* ====================================================== */

TEST_F(SubAgentTest, CreateContextBasic) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "ctx_test",
		sizeof(sa_cfg.entries[0].name) - 1);
	sa_cfg.entries[0].max_iterations = 3;
	sa_cfg.count = 1;
	sub_agent_runtime_load_config(rt, &sa_cfg);
	struct react_context *child = sub_agent_create_context(
		rt, &rt->entries[0], "test task");
	ASSERT_NE(child, nullptr);
	EXPECT_EQ(child->llm_model, llm);
	EXPECT_EQ(child->max_iterations, 3);
	EXPECT_EQ(child->sub_agent_depth, 1);
	react_context_destroy(child);
	tool_registry_cleanup(child->tools);
	free(child->tools);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, CreateContextSystemPrompt) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "prompted",
		sizeof(sa_cfg.entries[0].name) - 1);
	rt->entries[0].system_prompt = strdup("You are a specialist.");
	rt->entry_count = 1;
	struct react_context *child = sub_agent_create_context(
		rt, &rt->entries[0], "do something");
	ASSERT_NE(child, nullptr);
	ASSERT_NE(child->system_prompt, nullptr);
	EXPECT_STREQ(child->system_prompt, "You are a specialist.");
	react_context_destroy(child);
	tool_registry_cleanup(child->tools);
	free(child->tools);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, CreateContextDepthIncrement) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	rt->depth = 1;
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "deep",
		sizeof(sa_cfg.entries[0].name) - 1);
	sa_cfg.count = 1;
	sub_agent_runtime_load_config(rt, &sa_cfg);
	struct react_context *child = sub_agent_create_context(
		rt, &rt->entries[0], "nested task");
	ASSERT_NE(child, nullptr);
	EXPECT_EQ(child->sub_agent_depth, 2);
	react_context_destroy(child);
	tool_registry_cleanup(child->tools);
	free(child->tools);
	sub_agent_runtime_destroy(rt);
}

/* ====================================================== */
/* 7. Sync Invocation                                     */
/* ====================================================== */

TEST_F(SubAgentTest, InvokeSyncWithMockLlm) {
	tool_register(&tools, "text_gen", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "worker",
		sizeof(sa_cfg.entries[0].name) - 1);
	sa_cfg.entries[0].max_iterations = 3;
	sa_cfg.count = 1;
	sub_agent_runtime_load_config(rt, &sa_cfg);
	char *result = nullptr;
	int rc = sub_agent_invoke_sync(rt, &rt->entries[0],
				       "test task", &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result, nullptr);
	EXPECT_TRUE(strstr(result, "sub-agent result") != nullptr);
	free(result);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, InvokeSyncDepthLimit) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	rt->depth = SUB_AGENT_MAX_DEPTH;
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "deep",
		sizeof(sa_cfg.entries[0].name) - 1);
	sa_cfg.count = 1;
	sub_agent_runtime_load_config(rt, &sa_cfg);
	char *result = nullptr;
	int rc = sub_agent_invoke_sync(rt, &rt->entries[0],
				       "too deep", &result);
	EXPECT_EQ(rc, -ELOOP);
	free(result);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, InvokeSyncNullArgs) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	char *result = nullptr;
	EXPECT_EQ(sub_agent_invoke_sync(nullptr, nullptr, nullptr, &result),
		  -EINVAL);
	EXPECT_EQ(sub_agent_invoke_sync(rt, nullptr, "task", &result),
		  -EINVAL);
	EXPECT_EQ(sub_agent_invoke_sync(rt, &rt->entries[0], "task", nullptr),
		  -EINVAL);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, InvokeSyncLlmFailure) {
	struct model *fail_llm = create_mock_llm("error");
	struct mock_llm_data *fd = (struct mock_llm_data *)fail_llm->handle;
	fd->should_fail = 1;
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, fail_llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "failbot",
		sizeof(sa_cfg.entries[0].name) - 1);
	sa_cfg.entries[0].max_iterations = 2;
	sa_cfg.count = 1;
	sub_agent_runtime_load_config(rt, &sa_cfg);
	char *result = nullptr;
	int rc = sub_agent_invoke_sync(rt, &rt->entries[0],
				       "will fail", &result);
	EXPECT_NE(rc, 0);
	free(result);
	sub_agent_runtime_destroy(rt);
	model_destroy(fail_llm);
}

/* ====================================================== */
/* 8. Async Delegate + Check Status                       */
/* ====================================================== */

TEST_F(SubAgentTest, DelegateAndCheckStatus) {
	tool_register(&tools, "text_gen", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "async_worker",
		sizeof(sa_cfg.entries[0].name) - 1);
	sa_cfg.entries[0].max_iterations = 3;
	sa_cfg.count = 1;
	sub_agent_runtime_load_config(rt, &sa_cfg);
	char *task_id = nullptr;
	int rc = sub_agent_delegate(rt, "async_worker", "async task",
				    &task_id);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(task_id, nullptr);
	EXPECT_TRUE(strncmp(task_id, "sa_", 3) == 0);

	/* wait for completion */
	usleep(500000);
	enum sub_agent_task_status status = SUB_AGENT_PENDING;
	char *result = nullptr;
	rc = sub_agent_check_status(rt, task_id, &status, &result);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(status, SUB_AGENT_COMPLETED);
	ASSERT_NE(result, nullptr);
	EXPECT_TRUE(strstr(result, "sub-agent result") != nullptr);
	free(result);
	free(task_id);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, DelegateDepthLimit) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	rt->depth = SUB_AGENT_MAX_DEPTH;
	char *task_id = nullptr;
	int rc = sub_agent_delegate(rt, "any", "task", &task_id);
	EXPECT_EQ(rc, -ELOOP);
	free(task_id);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, DelegateInvalidAgent) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	char *task_id = nullptr;
	int rc = sub_agent_delegate(rt, "nonexistent", "task", &task_id);
	EXPECT_EQ(rc, -ENOENT);
	free(task_id);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, CheckStatusInvalidId) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	enum sub_agent_task_status status = SUB_AGENT_PENDING;
	char *result = nullptr;
	int rc = sub_agent_check_status(rt, "sa_999", &status, &result);
	EXPECT_EQ(rc, -ENOENT);
	sub_agent_runtime_destroy(rt);
}

/* ====================================================== */
/* 9. Fanout                                              */
/* ====================================================== */

TEST_F(SubAgentTest, FanoutRaw) {
	tool_register(&tools, "text_gen", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "fanout_worker",
		sizeof(sa_cfg.entries[0].name) - 1);
	sa_cfg.entries[0].max_iterations = 3;
	sa_cfg.count = 1;
	sub_agent_runtime_load_config(rt, &sa_cfg);
	const char *tasks[] = {"task A", "task B"};
	char *result = nullptr;
	int rc = sub_agent_fanout(rt, "fanout_worker", tasks, 2,
				  SUB_AGENT_MERGE_RAW, &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result, nullptr);
	cJSON *arr = cJSON_Parse(result);
	ASSERT_NE(arr, nullptr);
	EXPECT_EQ(cJSON_GetArraySize(arr), 2);
	cJSON_Delete(arr);
	free(result);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, FanoutConcat) {
	tool_register(&tools, "text_gen", "desc", "{}",
		      sa_test_tool_fn, NULL, NULL);
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	struct config_sub_agents sa_cfg = {};
	strncpy(sa_cfg.entries[0].name, "concat_worker",
		sizeof(sa_cfg.entries[0].name) - 1);
	sa_cfg.entries[0].max_iterations = 3;
	sa_cfg.count = 1;
	sub_agent_runtime_load_config(rt, &sa_cfg);
	const char *tasks[] = {"single task"};
	char *result = nullptr;
	int rc = sub_agent_fanout(rt, "concat_worker", tasks, 1,
				  SUB_AGENT_MERGE_CONCAT, &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result, nullptr);
	free(result);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, FanoutDepthLimit) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	rt->depth = SUB_AGENT_MAX_DEPTH;
	const char *tasks[] = {"x"};
	char *result = nullptr;
	int rc = sub_agent_fanout(rt, "any", tasks, 1,
				  SUB_AGENT_MERGE_RAW, &result);
	EXPECT_EQ(rc, -ELOOP);
	free(result);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, FanoutInvalidAgent) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	const char *tasks[] = {"x"};
	char *result = nullptr;
	int rc = sub_agent_fanout(rt, "ghost", tasks, 1,
				  SUB_AGENT_MERGE_RAW, &result);
	EXPECT_EQ(rc, -ENOENT);
	free(result);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, FanoutNullTasks) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	char *result = nullptr;
	EXPECT_EQ(sub_agent_fanout(rt, "a", nullptr, 1,
				   SUB_AGENT_MERGE_RAW, &result), -EINVAL);
	EXPECT_EQ(sub_agent_fanout(rt, "a", nullptr, 0,
				   SUB_AGENT_MERGE_RAW, &result), -EINVAL);
	sub_agent_runtime_destroy(rt);
}

/* ====================================================== */
/* 10. Output Schema                                      */
/* ====================================================== */

TEST_F(SubAgentTest, OutputSchemaNullArgs) {
	EXPECT_EQ(sub_agent_apply_output_schema(nullptr, "s", llm, nullptr),
		  -EINVAL);
	EXPECT_EQ(sub_agent_apply_output_schema("text", nullptr, llm, nullptr),
		  -EINVAL);
}

TEST_F(SubAgentTest, OutputSchemaNoLlm) {
	char *result = nullptr;
	int rc = sub_agent_apply_output_schema("some text",
		"{\"type\":\"object\"}", nullptr, &result);
	EXPECT_EQ(rc, 0);
	ASSERT_NE(result, nullptr);
	EXPECT_STREQ(result, "some text");
	free(result);
}

/* ====================================================== */
/* 11. Active Context Stack (Phase 1 regression)          */
/* ====================================================== */

TEST_F(SubAgentTest, ActiveCtxStackPushPop) {
	struct react_context *ctx1 = react_context_create(
		&tools, tok, &cfg, nullptr);
	struct react_context *ctx2 = react_context_create(
		&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx1, nullptr);
	ASSERT_NE(ctx2, nullptr);
	EXPECT_EQ(react_active_count(), 0);
	react_active_push(ctx1);
	EXPECT_EQ(react_active_count(), 1);
	react_active_push(ctx2);
	EXPECT_EQ(react_active_count(), 2);
	react_active_pop(ctx1);
	EXPECT_EQ(react_active_count(), 1);
	react_active_pop(ctx2);
	EXPECT_EQ(react_active_count(), 0);
	react_context_destroy(ctx1);
	react_context_destroy(ctx2);
}

TEST_F(SubAgentTest, CancelActiveCancelsAll) {
	struct react_context *ctx1 = react_context_create(
		&tools, tok, &cfg, nullptr);
	struct react_context *ctx2 = react_context_create(
		&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx1, nullptr);
	ASSERT_NE(ctx2, nullptr);
	react_active_push(ctx1);
	react_active_push(ctx2);
	EXPECT_EQ(ctx1->cancelled, 0);
	EXPECT_EQ(ctx2->cancelled, 0);
	react_cancel_active();
	EXPECT_EQ(ctx1->cancelled, 1);
	EXPECT_EQ(ctx2->cancelled, 1);
	react_active_pop(ctx1);
	react_active_pop(ctx2);
	react_context_destroy(ctx1);
	react_context_destroy(ctx2);
}

TEST_F(SubAgentTest, CancelActiveEmpty) {
	EXPECT_EQ(react_active_count(), 0);
	EXPECT_NO_FATAL_FAILURE(react_cancel_active());
}

/* ====================================================== */
/* 12. System Prompt Integration                          */
/* ====================================================== */

TEST_F(SubAgentTest, SystemPromptSubAgentList) {
	struct react_context *ctx = react_context_create(
		&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->sub_agent_info = (decltype(ctx->sub_agent_info))calloc(
		2, sizeof(*ctx->sub_agent_info));
	ctx->sub_agent_info_count = 2;
	strncpy(ctx->sub_agent_info[0].name, "researcher",
		sizeof(ctx->sub_agent_info[0].name) - 1);
	strncpy(ctx->sub_agent_info[0].description, "Research agent",
		sizeof(ctx->sub_agent_info[0].description) - 1);
	strncpy(ctx->sub_agent_info[1].name, "coder",
		sizeof(ctx->sub_agent_info[1].name) - 1);
	strncpy(ctx->sub_agent_info[1].description, "Code writer",
		sizeof(ctx->sub_agent_info[1].description) - 1);

	/* Verify sub_agent_info is set correctly */
	EXPECT_STREQ(ctx->sub_agent_info[0].name, "researcher");
	EXPECT_STREQ(ctx->sub_agent_info[1].name, "coder");
	EXPECT_EQ(ctx->sub_agent_info_count, 2);

	free(ctx->sub_agent_info);
	ctx->sub_agent_info = nullptr;
	ctx->sub_agent_info_count = 0;
	react_context_destroy(ctx);
}

TEST_F(SubAgentTest, SystemPromptNoSubAgents) {
	struct react_context *ctx = react_context_create(
		&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->sub_agent_info_count, 0);
	EXPECT_EQ(ctx->sub_agent_info, nullptr);
	react_context_destroy(ctx);
}

/* ====================================================== */
/* 13. Tracing                                            */
/* ====================================================== */

TEST_F(SubAgentTest, TraceWrite) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	/* Use /tmp for trace file to ensure write works */
	snprintf(rt->trace_file, sizeof(rt->trace_file),
		 "/tmp/ma_sa_trace_%d.jsonl", getpid());
	struct sub_agent_trace_event ev = {};
	strncpy(ev.trace_id, "t_test", sizeof(ev.trace_id) - 1);
	strncpy(ev.agent_name, "test_agent",
		sizeof(ev.agent_name) - 1);
	strncpy(ev.mode, "tool", sizeof(ev.mode) - 1);
	ev.start_ms = 1000;
	ev.end_ms = 2000;
	ev.iteration_count = 3;
	ev.result_preview = strdup("preview text");
	EXPECT_NO_FATAL_FAILURE(sub_agent_trace_write(rt, &ev));
	free(ev.result_preview);
	/* Verify file exists and contains valid JSON */
	FILE *f = fopen(rt->trace_file, "r");
	ASSERT_NE(f, nullptr);
	char line[4096];
	ASSERT_NE(fgets(line, sizeof(line), f), nullptr);
	fclose(f);
	cJSON *obj = cJSON_Parse(line);
	ASSERT_NE(obj, nullptr);
	EXPECT_NE(cJSON_GetObjectItem(obj, "trace_id"), nullptr);
	EXPECT_NE(cJSON_GetObjectItem(obj, "agent"), nullptr);
	EXPECT_NE(cJSON_GetObjectItem(obj, "iterations"), nullptr);
	cJSON_Delete(obj);
	std::remove(rt->trace_file);
	sub_agent_runtime_destroy(rt);
}

TEST_F(SubAgentTest, TraceNullRuntime) {
	struct sub_agent_trace_event ev = {};
	EXPECT_NO_FATAL_FAILURE(sub_agent_trace_write(nullptr, &ev));
}

TEST_F(SubAgentTest, TraceNullEvent) {
	struct sub_agent_runtime *rt = sub_agent_runtime_create(
		&tools, llm, tok, &cfg);
	EXPECT_NO_FATAL_FAILURE(sub_agent_trace_write(rt, nullptr));
	sub_agent_runtime_destroy(rt);
}

/* ====================================================== */
/* 14. Sub-Agent Depth in react_context                   */
/* ====================================================== */

TEST_F(SubAgentTest, ReactContextSubAgentDepthDefault) {
	struct react_context *ctx = react_context_create(
		&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->sub_agent_depth, 0);
	react_context_destroy(ctx);
}

TEST_F(SubAgentTest, ReactContextSubAgentDepthSet) {
	struct react_context *ctx = react_context_create(
		&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->sub_agent_depth = 1;
	EXPECT_EQ(ctx->sub_agent_depth, 1);
	react_reset(ctx);
	/* reset does NOT clear sub_agent_depth (it's a structural property) */
	react_context_destroy(ctx);
}
