#include <gtest/gtest.h>

#include "models/llm.h"
#include "models/llm/codec.h"
extern "C" {
#include "models/llm/chat_codec.h"
}
#include "models/llm/stream.h"
#include "util/cleanup.h"
#include "util/json.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>

TEST(TestModelOwnership, MessageCloneOutlivesBorrowedSource)
{
	struct chat_message copy MORPH_CLEANUP(chat_message_cleanup) = {};
	{
		char role[] = "assistant";
		char content[] = "answer";
		char reasoning[] = "reasoning";
		char arguments[] = "{\"value\":1}";
		char call_id[] = "call_1";
		struct tool_call call{};
		std::strcpy(call.id, call_id);
		std::strcpy(call.tool_call_id, "provider_item");
		std::strcpy(call.name, "example");
		call.arguments = arguments;
		call.input_kind = TOOL_INPUT_TEXT;
		struct chat_message source{};
		source.role = role;
		source.content = content;
		source.reasoning_content = reasoning;
		source.tool_call_id = call_id;
		source.tool_calls = &call;
		source.tool_call_count = 1;
		source.is_context_summary = 1;
		ASSERT_EQ(chat_message_clone(&source, &copy), 0);
		EXPECT_NE(copy.content, source.content);
		EXPECT_NE(copy.tool_calls, source.tool_calls);
		EXPECT_NE(copy.tool_calls[0].arguments, call.arguments);
		content[0] = 'X';
		arguments[0] = 'X';
	}
	EXPECT_STREQ(copy.role, "assistant");
	EXPECT_STREQ(copy.content, "answer");
	EXPECT_STREQ(copy.reasoning_content, "reasoning");
	EXPECT_STREQ(copy.tool_call_id, "call_1");
	ASSERT_EQ(copy.tool_call_count, 1);
	EXPECT_STREQ(copy.tool_calls[0].arguments, "{\"value\":1}");
	EXPECT_STREQ(copy.tool_calls[0].tool_call_id, "provider_item");
	EXPECT_EQ(copy.tool_calls[0].input_kind, TOOL_INPUT_TEXT);
	EXPECT_EQ(copy.is_context_summary, 1);
}

TEST(TestModelOwnership, InvalidMessageCloneLeavesDestinationEmpty)
{
	struct chat_message source{};
	struct chat_message copy MORPH_CLEANUP(chat_message_cleanup) = {};
	source.tool_call_count = 1;
	EXPECT_EQ(chat_message_clone(&source, &copy), -EINVAL);
	EXPECT_EQ(copy.role, nullptr);
	EXPECT_EQ(copy.tool_calls, nullptr);
	EXPECT_EQ(copy.tool_call_count, 0);
	source.tool_call_count = -1;
	EXPECT_EQ(chat_message_clone(&source, &copy), -EINVAL);
	EXPECT_EQ(copy.content, nullptr);
}

TEST(TestModelOwnership, CloningRejectsAnOccupiedOwnerWithoutChangingEitherObject)
{
	struct chat_message source{};
	char source_content[] = "source";
	source.content = source_content;
	struct chat_message owned MORPH_CLEANUP(chat_message_cleanup) = {};
	owned.content = strdup("original");
	ASSERT_NE(owned.content, nullptr);
	char *original = owned.content;
	EXPECT_EQ(chat_message_clone(&source, &owned), -EEXIST);
	EXPECT_EQ(owned.content, original);
	EXPECT_STREQ(owned.content, "original");
	EXPECT_STREQ(source.content, "source");
	struct tool_call call{};
	char input[] = "new input";
	call.arguments = input;
	struct tool_call existing MORPH_CLEANUP(tool_call_cleanup) = {};
	existing.arguments = strdup("old input");
	ASSERT_NE(existing.arguments, nullptr);
	char *old_input = existing.arguments;
	EXPECT_EQ(tool_call_clone(&call, &existing), -EEXIST);
	EXPECT_EQ(existing.arguments, old_input);
	EXPECT_STREQ(existing.arguments, "old input");
}

TEST(TestModelOwnership, StreamTransfersCallsWithoutRetainingAnOwner)
{
	struct chat_response response MORPH_CLEANUP(chat_response_free) = {};
	struct tool_call *transferred;
	{
		struct llm_stream_ctx stream MORPH_CLEANUP(llm_stream_cleanup) = {};
		llm_stream_init(&stream, nullptr, nullptr);
		struct tool_call *call = llm_stream_tool_call_ensure_borrow(&stream, 0);
		ASSERT_NE(call, nullptr);
		std::strcpy(call->id, "call_1");
		call->arguments = strdup("{\"ok\":true}");
		ASSERT_NE(call->arguments, nullptr);
		transferred = stream.tool_calls;
		llm_stream_tool_calls_move(&stream, &response);
		EXPECT_EQ(stream.tool_calls, nullptr);
		EXPECT_EQ(stream.tool_call_count, 0);
		EXPECT_EQ(stream.tool_call_cap, 0);
	}
	ASSERT_EQ(response.tool_calls, transferred);
	ASSERT_EQ(response.tool_call_count, 1);
	EXPECT_STREQ(response.tool_calls[0].id, "call_1");
	EXPECT_STREQ(response.tool_calls[0].arguments, "{\"ok\":true}");
	chat_response_free(&response);
	EXPECT_EQ(response.tool_calls, nullptr);
	EXPECT_EQ(response.tool_call_count, 0);
}

TEST(TestModelOwnership, StreamRejectsInvalidIndicesWithoutChangingCalls)
{
	struct llm_stream_ctx stream MORPH_CLEANUP(llm_stream_cleanup) = {};
	llm_stream_init(&stream, nullptr, nullptr);
	EXPECT_EQ(llm_stream_tool_call_ensure_borrow(&stream, -1), nullptr);
	EXPECT_EQ(llm_stream_tool_call_ensure_borrow(&stream, INT_MAX), nullptr);
	EXPECT_EQ(stream.tool_calls, nullptr);
	EXPECT_EQ(stream.tool_call_count, 0);
	EXPECT_EQ(stream.tool_call_cap, 0);
}

TEST(TestModelOwnership, StreamCallMoveReplacesAnExistingDestinationOwner)
{
	struct chat_response response MORPH_CLEANUP(chat_response_free) = {};
	response.content = strdup("existing content");
	response.tool_calls = static_cast<tool_call *>(calloc(1, sizeof(*response.tool_calls)));
	ASSERT_NE(response.content, nullptr);
	ASSERT_NE(response.tool_calls, nullptr);
	response.tool_call_count = 1;
	response.tool_calls[0].arguments = strdup("old arguments");
	ASSERT_NE(response.tool_calls[0].arguments, nullptr);
	struct llm_stream_ctx stream MORPH_CLEANUP(llm_stream_cleanup) = {};
	llm_stream_init(&stream, nullptr, nullptr);
	struct tool_call *call = llm_stream_tool_call_ensure_borrow(&stream, 0);
	ASSERT_NE(call, nullptr);
	call->arguments = strdup("new arguments");
	ASSERT_NE(call->arguments, nullptr);
	struct tool_call *incoming = stream.tool_calls;
	llm_stream_tool_calls_move(&stream, &response);
	EXPECT_EQ(response.tool_calls, incoming);
	ASSERT_EQ(response.tool_call_count, 1);
	EXPECT_STREQ(response.tool_calls[0].arguments, "new arguments");
	EXPECT_STREQ(response.content, "existing content");
	EXPECT_EQ(stream.tool_calls, nullptr);
	EXPECT_EQ(stream.tool_call_count, 0);
}

TEST(TestModelOwnership, SchemaCopyDoesNotNormalizeOrConsumeItsObjectSource)
{
	cJSON *source MORPH_CLEANUP(morph_json_cleanup) = cJSON_Parse(
		"{\"description\":\"source\",\"properties\":{\"value\":{\"type\":\"string\"}}}");
	ASSERT_NE(source, nullptr);
	cJSON *copy MORPH_CLEANUP(morph_json_cleanup) = llm_codec_schema_copy(source);
	ASSERT_NE(copy, nullptr);
	EXPECT_NE(copy, source);
	EXPECT_FALSE(cJSON_HasObjectItem(source, "type"));
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(copy, "type")), "object");
	cJSON_SetValuestring(cJSON_GetObjectItem(source, "description"), "changed");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(copy, "description")), "source");
	morph_json_cleanup(&source);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(copy, "description")), "source");
}

TEST(TestModelOwnership, SchemaCopyOwnsAnIndependentProjectionOfAnArraySource)
{
	cJSON *source MORPH_CLEANUP(morph_json_cleanup) =
		cJSON_Parse("[{\"name\":\"value\",\"description\":\"source\",\"required\":true}]");
	ASSERT_NE(source, nullptr);
	cJSON *copy MORPH_CLEANUP(morph_json_cleanup) = llm_codec_schema_copy(source);
	ASSERT_NE(copy, nullptr);
	EXPECT_TRUE(cJSON_IsArray(source));
	ASSERT_EQ(cJSON_GetArraySize(source), 1);
	cJSON *source_item = cJSON_GetArrayItem(source, 0);
	cJSON_SetValuestring(cJSON_GetObjectItem(source_item, "name"), "changed");
	morph_json_cleanup(&source);
	cJSON *properties = cJSON_GetObjectItem(copy, "properties");
	cJSON *value = cJSON_GetObjectItem(properties, "value");
	ASSERT_NE(value, nullptr);
	EXPECT_FALSE(cJSON_HasObjectItem(properties, "changed"));
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(value, "description")), "source");
	cJSON *required = cJSON_GetObjectItem(copy, "required");
	ASSERT_EQ(cJSON_GetArraySize(required), 1);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetArrayItem(required, 0)), "value");
}

TEST(TestModelOwnership, SchemaCopyRejectsInvalidInputWithoutConsumingIt)
{
	cJSON *source MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateString("not a schema");
	ASSERT_NE(source, nullptr);
	EXPECT_EQ(llm_codec_schema_copy(source), nullptr);
	EXPECT_TRUE(cJSON_IsString(source));
	EXPECT_STREQ(cJSON_GetStringValue(source), "not a schema");
	EXPECT_EQ(llm_codec_schema_copy(nullptr), nullptr);
}

TEST(TestModelOwnership, ChatBodyCreationRejectsAnOccupiedOwner)
{
	cJSON *value MORPH_CLEANUP(morph_json_cleanup) = cJSON_CreateString("original");
	ASSERT_NE(value, nullptr);
	char *body MORPH_CLEANUP(morph_json_text_cleanup) = morph_json_print_copy(value);
	ASSERT_NE(body, nullptr);
	char *original = body;
	size_t length = 37;
	EXPECT_EQ(llm_chat_body_create(nullptr, "model", nullptr, nullptr, 0, 0, 0, nullptr,
				       nullptr, 0, &body, &length),
		  -EEXIST);
	EXPECT_EQ(body, original);
	EXPECT_EQ(length, 37U);
	EXPECT_STREQ(body, "\"original\"");
}
