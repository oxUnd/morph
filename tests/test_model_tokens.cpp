#include <gtest/gtest.h>
#include "models/llm.h"
#include "agent/tokenizer.h"
#include "util/error.h"
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
	std::strcpy(call.id, "provider_1");
	std::strcpy(call.name, "probe");
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
	EXPECT_EQ(model_estimate_input_tokens(&model, "", nullptr, 1,
		nullptr, 0, count_input_text, nullptr, &tokens), -EINVAL);
}
