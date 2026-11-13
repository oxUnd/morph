#include <gtest/gtest.h>
#include "models/llm.h"
#include "models/image_provider.h"
#include "nb/cleanup.h"
#include "agent/tokenizer.h"
#include "agent/react_request.h"
#include "errors.h"
#include <climits>
#include <cstring>
#include <string>

static int count_input_text(const char *text, void *user)
{
	return tokenizer_count(static_cast<struct tokenizer *>(user), text);
}

static int input_tokens(const char *adapter, struct chat_message *messages,
	int message_count, struct tool_desc *tools = nullptr, int tool_count = 0)
{
	struct model model = {};
	int tokens = 0;

	std::strncpy(model.adapter, adapter, sizeof(model.adapter) - 1);
	EXPECT_EQ(model_estimate_input_tokens(&model, "System instructions.",
		messages, message_count, tools, tool_count, count_input_text,
		nullptr, &tokens), 0);
	return tokens;
}

TEST(ModelTokenBudgetTest, IgnoresToolFieldsAbsentFromBothWireFormats)
{
	std::string large(20000, '!');
	struct tool_desc tool = {};
	tool.name = "probe";
	tool.description = "Probe an endpoint";
	tool.input_schema = "{\"type\":\"object\",\"properties\":{}}";
	for (const char *adapter : {"openai-chat-compatible", "openai-responses", "deepseek"}) {
		tool.title = nullptr;
		tool.output_schema = nullptr;
		int before = input_tokens(adapter, nullptr, 0, &tool, 1);
		tool.title = large.c_str();
		tool.output_schema = large.c_str();
		EXPECT_EQ(input_tokens(adapter, nullptr, 0, &tool, 1), before);
		tool.description = large.c_str();
		EXPECT_GT(input_tokens(adapter, nullptr, 0, &tool, 1), before);
		tool.description = "Probe an endpoint";
	}
}

TEST(ModelInputSnapshotTest, UsesAdapterWireFieldsWithoutHeadersOrGenerationOptions)
{
	for (const char *adapter : {"openai-chat-compatible", "openai-responses"}) {
		struct model model{};
		strncpy(model.adapter, adapter, sizeof(model.adapter) - 1);
		strcpy(model.api_key, "never-serialize-this-header-secret");
		struct chat_message message{};
		message.role = const_cast<char *>("user");
		message.content = const_cast<char *>("visible input 中文");
		cJSON *input = nullptr;
		ASSERT_EQ(model_input_snapshot_create(&model, "system instructions", &message, 1,
						      nullptr, 0, &input),
			  0);
		bool responses = std::string(adapter) == "openai-responses";
		EXPECT_TRUE(cJSON_IsArray(cJSON_GetObjectItem(input, responses ? "input" : "messages")));
		EXPECT_EQ(cJSON_HasObjectItem(input, "instructions"), responses);
		EXPECT_FALSE(cJSON_HasObjectItem(input, "max_tokens"));
		char *json = cJSON_PrintUnformatted(input);
		ASSERT_NE(json, nullptr);
		EXPECT_EQ(strstr(json, model.api_key), nullptr);
		EXPECT_NE(strstr(json, "system instructions"), nullptr);
		free(json);
		cJSON_Delete(input);
	}
}

TEST(ModelTokenBudgetTest, CountsReasoningOnlyForAdapterThatSendsIt)
{
	std::string reasoning(20000, '!');
	struct chat_message message = {};
	message.role = const_cast<char *>("assistant");
	message.content = const_cast<char *>("Answer");
	for (const char *adapter : {"openai-chat-compatible", "openai-responses", "deepseek"}) {
		message.reasoning_content = nullptr;
		int before = input_tokens(adapter, &message, 1);
		message.reasoning_content = reasoning.data();
		int after = input_tokens(adapter, &message, 1);
		if (std::strcmp(adapter, "deepseek") == 0)
			EXPECT_GT(after, before);
		else
			EXPECT_EQ(after, before);
	}
}

TEST(ModelTokenBudgetTest, FreeformToolsUseAdapterSpecificInputDefinition)
{
	struct tool_desc tool = {};
	tool.name = "patch";
	tool.description = "Apply a patch";
	tool.input_kind = TOOL_INPUT_TEXT;
	tool.input_format = "{\"type\":\"text\"}";
	int chat = input_tokens("openai-chat-compatible", nullptr, 0, &tool, 1);
	int responses = input_tokens("openai-responses", nullptr, 0, &tool, 1);
	std::string ignored = "{\"description\":\"" + std::string(20000, '!') + "\"}";
	tool.input_schema = ignored.c_str();
	EXPECT_EQ(input_tokens("openai-chat-compatible", nullptr, 0, &tool, 1), chat);
	EXPECT_EQ(input_tokens("openai-responses", nullptr, 0, &tool, 1), responses);
	tool.input_format = ignored.c_str();
	EXPECT_EQ(input_tokens("openai-chat-compatible", nullptr, 0, &tool, 1), chat);
	EXPECT_GT(input_tokens("openai-responses", nullptr, 0, &tool, 1), responses);
}

TEST(ModelTokenBudgetTest, IncludesToolArgumentsAndResults)
{
	struct tool_call call = {};
	call.id = const_cast<char *>("provider_1");
	call.name = const_cast<char *>("probe");
	call.arguments = const_cast<char *>("{}");
	struct chat_message messages[2] = {};
	messages[0].role = const_cast<char *>("assistant");
	messages[0].tool_calls = &call;
	messages[0].tool_call_count = 1;
	messages[1].role = const_cast<char *>("tool");
	messages[1].tool_call_id = call.id;
	messages[1].content = const_cast<char *>("OK");
	std::string large(20000, '!');
	for (const char *adapter : {"openai-chat-compatible", "openai-responses"}) {
		call.arguments = const_cast<char *>("{}");
		messages[1].content = const_cast<char *>("OK");
		int before = input_tokens(adapter, messages, 2);
		call.arguments = large.data();
		EXPECT_GT(input_tokens(adapter, messages, 2), before);
		call.arguments = const_cast<char *>("{}");
		messages[1].content = large.data();
		EXPECT_GT(input_tokens(adapter, messages, 2), before);
	}
}

TEST(ModelTokenBudgetTest, SaturatesLargeEstimatesAndRejectsInvalidArrays)
{
	struct model model = {};
	int tokens = 0;
	auto large_counter = [](const char *, void *) { return INT_MAX; };
	EXPECT_EQ(model_estimate_input_tokens(&model, "instructions", nullptr, 0,
		nullptr, 0, large_counter, nullptr, &tokens), 0);
	EXPECT_EQ(tokens, INT_MAX);
	EXPECT_EQ(model_estimate_input_tokens(&model, "", nullptr, 1, nullptr, 0, count_input_text,
					      nullptr, &tokens),
		  NB_ERR_INVAL);
}

TEST(ModelTokenBudgetTest, ReservesFullOutputAndProtocolWithoutHalfWindowClamp)
{
	struct react_context context{};
	struct model model{};
	context.compress.max_context_tokens = 128000;
	context.compress.protocol_reserve_tokens = 4096;
	model.context_limit = 128000;
	model.max_tokens = 100000;
	EXPECT_EQ(react_request_input_budget(&context, &model), 23904);
	model.max_tokens = 16384;
	EXPECT_EQ(react_request_input_budget(&context, &model), 107520);
	model.context_limit = 32000;
	EXPECT_EQ(react_request_input_budget(&context, &model), 11520);
	model.max_tokens = 32000;
	EXPECT_EQ(react_request_input_budget(&context, &model), 0);
	context.compress.max_context_tokens = INT_MAX;
	context.compress.protocol_reserve_tokens = INT_MAX;
	model.context_limit = INT_MAX;
	model.max_tokens = INT_MAX;
	EXPECT_EQ(react_request_input_budget(&context, &model), 0);
	context.compress.max_context_tokens = 0;
	context.compress.protocol_reserve_tokens = 4096;
	model.context_limit = 128000;
	model.max_tokens = 100000;
	EXPECT_EQ(react_request_input_budget(&context, &model), 23904);
}

TEST(ModelCloneTest, CopiesConfigurationAndResetsRequestState)
{
	model *source = model_llm_create(nullptr, "gateway", "model", "http://unit", "key");
	ASSERT_NE(source, nullptr);
	strcpy(source->name, "configured");
	strcpy(source->adapter, "openai-responses");
	strcpy(source->extra_body_json, "{\"temperature\":0.2}");
	source->context_limit = 42000;
	source->max_tokens = 512;
	source->timeout_seconds = 7;
	source->retry_count = 2;
	source->poll_interval_seconds = 3;
	source->poll_timeout_seconds = 17;
	nb_cancel cancelled{};
	model_usage_sink sink{};
	source->http.cancel_token = &cancelled;
	source->usage_sink = &sink;
	strcpy(source->last_error, "previous error");
	source->error_info.kind = MODEL_ERROR_API;
	model *copy = model_llm_clone(source, nullptr);
	ASSERT_NE(copy, nullptr);
	EXPECT_STREQ(copy->name, source->name);
	EXPECT_STREQ(copy->adapter, source->adapter);
	EXPECT_STREQ(copy->extra_body_json, source->extra_body_json);
	EXPECT_STREQ(copy->provider, source->provider);
	EXPECT_STREQ(copy->model_id, source->model_id);
	EXPECT_STREQ(copy->api_base, source->api_base);
	EXPECT_STREQ(copy->api_key, source->api_key);
	EXPECT_EQ(copy->context_limit, 42000);
	EXPECT_EQ(copy->max_tokens, 512);
	EXPECT_EQ(copy->timeout_seconds, 7);
	EXPECT_EQ(copy->retry_count, 2);
	EXPECT_EQ(copy->poll_interval_seconds, 3);
	EXPECT_EQ(copy->poll_timeout_seconds, 17);
	EXPECT_EQ(copy->http.cancel_token, nullptr);
	EXPECT_EQ(copy->usage_sink, nullptr);
	EXPECT_EQ(copy->last_error[0], '\0');
	EXPECT_EQ(copy->error_info.kind, MODEL_ERROR_NONE);
	EXPECT_EQ(copy->handle, nullptr);
	model_destroy(source);
	EXPECT_STREQ(copy->adapter, "openai-responses");
	model_destroy(copy);
	EXPECT_EQ(model_llm_clone(nullptr, nullptr), nullptr);
}


TEST(ModelCredentialTest, LongCredentialClonesOwnIndependentCompleteValues)
{
	std::string key = "mock-" + std::string(4096, 'k');
	auto *source = model_llm_create(nullptr, "test", "mock", nullptr, key.c_str());
	ASSERT_NE(source, nullptr);
	EXPECT_EQ(source->api_key[0], '\0');
	EXPECT_STREQ(model_api_key_borrow(source), key.c_str());
	auto *copy = model_llm_clone(source, nullptr);
	ASSERT_NE(copy, nullptr);
	EXPECT_NE(source->api_key_owned, copy->api_key_owned);
	model_destroy(source);
	EXPECT_STREQ(model_api_key_borrow(copy), key.c_str());
	nb_buf header NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_EQ(model_authorization_header_create(copy, &header), 0);
	EXPECT_EQ(std::string(nb_buf_cstr(&header)), "Authorization: Bearer " + key);
	model_destroy(copy);
}

TEST(ModelCredentialTest, ImageRequestMetadataPreservesTheCompleteCredential)
{
	std::string key = "mock-" + std::string(2048, 'k');
	auto *model = model_llm_create(nullptr, "openai", "image", "https://example.test/v1",
		key.c_str());
	ASSERT_NE(model, nullptr);
	nb_buf url NB_CLEANUP(nb_buf_cleanup) = {};
	nb_buf authorization NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_EQ(image_provider_build_request_meta(model, "/images/generations", &url,
		&authorization), 0);
	EXPECT_STREQ(nb_buf_cstr(&url), "https://example.test/v1/images/generations");
	EXPECT_EQ(std::string(nb_buf_cstr(&authorization)), "Authorization: Bearer " + key);
	model_destroy(model);
}
