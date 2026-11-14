#include <gtest/gtest.h>
#include "nb/err.h"

#include "models/llm.h"
#include "models/llm/codec.h"
extern "C" {
#include "models/llm/chat_codec.h"
}
#include "models/llm/stream.h"
#include "nb/cleanup.h"
#include "cJSON.h"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>

TEST(TestModelOwnership, MessageCloneOutlivesBorrowedSource)
{
	struct chat_message copy NB_CLEANUP(chat_message_cleanup) = {};
	{
		char role[] = "assistant";
		char content[] = "answer";
		char reasoning[] = "reasoning";
		char arguments[] = "{\"value\":1}";
		char call_id[] = "call_1";
		struct tool_call call{};
		call.id = const_cast<char *>(call_id);
		call.tool_call_id = const_cast<char *>("provider_item");
		call.name = const_cast<char *>("example");
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

TEST(TestModelOwnership, ChatStreamAndClonePreserveLongToolIdentities)
{
	for (size_t size : {size_t{64}, size_t{128}, size_t{4096}}) {
		SCOPED_TRACE(size);
		std::string name(size, 'n');
		std::string id(size + 128, 'i');
		std::string event = "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
			"\"id\":\"" + id + "\",\"function\":{\"name\":\"" + name +
			"\",\"arguments\":\"{}\"}}]}}]}";
		struct tool_call copy NB_CLEANUP(tool_call_cleanup) = {};
		{
			struct llm_stream_ctx stream NB_CLEANUP(llm_stream_cleanup) = {};
			llm_stream_init(&stream, nullptr, nullptr);
			ASSERT_EQ(llm_chat_decode_event("message", event.c_str(), &stream), 0);
			ASSERT_EQ(stream.tool_call_count, 1);
			EXPECT_STREQ(stream.tool_calls[0].id, id.c_str());
			EXPECT_STREQ(stream.tool_calls[0].name, name.c_str());
			ASSERT_EQ(tool_call_clone(&stream.tool_calls[0], &copy), 0);
		}
		EXPECT_STREQ(copy.id, id.c_str());
		EXPECT_STREQ(copy.name, name.c_str());
		EXPECT_STREQ(copy.arguments, "{}");
	}
}

TEST(TestModelOwnership, ResponsesKeepItemAndCallIdsAcrossInterleavedDeltas)
{
	struct model model{};
	strcpy(model.adapter, "openai-responses");
	const struct llm_codec *codec = llm_codec_borrow(&model);
	struct llm_stream_ctx stream NB_CLEANUP(llm_stream_cleanup) = {};
	llm_stream_init(&stream, nullptr, nullptr);
	std::string prefix(1024, 'i');
	std::string name(512, 'n');
	for (int i = 0; i < 2; i++) {
		std::string suffix = std::to_string(i);
		std::string event = "{\"type\":\"response.output_item.added\",\"item\":{"
			"\"type\":\"function_call\",\"id\":\"" + prefix + suffix +
			"\",\"call_id\":\"call_" + prefix + suffix + "\",\"name\":\"" + name +
			"\",\"arguments\":\"\"}}";
		ASSERT_EQ(codec->event("message", event.c_str(), &stream), 0);
	}
	for (int i : {1, 0}) {
		std::string suffix = std::to_string(i);
		std::string delta = "{\"type\":\"response.function_call_arguments.delta\","
			"\"item_id\":\"" + prefix + suffix + "\",\"delta\":\"{}\"}";
		ASSERT_EQ(codec->event("message", delta.c_str(), &stream), 0);
		std::string done = "{\"type\":\"response.output_item.done\",\"item\":{"
			"\"type\":\"function_call\",\"id\":\"" + prefix + suffix +
			"\",\"call_id\":\"call_" + prefix + suffix + "\",\"name\":\"" + name +
			"\",\"arguments\":\"{}\"}}";
		ASSERT_EQ(codec->event("message", done.c_str(), &stream), 0);
	}
	ASSERT_EQ(stream.tool_call_count, 2);
	for (int i = 0; i < 2; i++) {
		std::string item = prefix + std::to_string(i);
		EXPECT_STREQ(stream.tool_calls[i].provider_item_id, item.c_str());
		EXPECT_EQ(std::string(stream.tool_calls[i].id), "call_" + item);
		EXPECT_STREQ(stream.tool_calls[i].name, name.c_str());
		EXPECT_STREQ(stream.tool_calls[i].arguments, "{}");
	}
	EXPECT_EQ(codec->event("message",
			       "{\"type\":\"response.function_call_arguments.delta\","
			       "\"item_id\":\"unknown\",\"delta\":\"wrong\"}",
			       &stream),
		  NB_ERR_PROTOCOL);
	EXPECT_EQ(codec->event("message",
			       "{\"type\":\"response.function_call_arguments.delta\","
			       "\"delta\":\"ambiguous\"}",
			       &stream),
		  NB_ERR_PROTOCOL);
}

TEST(TestModelOwnership, InvalidMessageCloneLeavesDestinationEmpty)
{
	struct chat_message source{};
	struct chat_message copy NB_CLEANUP(chat_message_cleanup) = {};
	source.tool_call_count = 1;
	EXPECT_EQ(chat_message_clone(&source, &copy), NB_ERR_INVAL);
	EXPECT_EQ(copy.role, nullptr);
	EXPECT_EQ(copy.tool_calls, nullptr);
	EXPECT_EQ(copy.tool_call_count, 0);
	source.tool_call_count = -1;
	EXPECT_EQ(chat_message_clone(&source, &copy), NB_ERR_INVAL);
	EXPECT_EQ(copy.content, nullptr);
}

TEST(TestModelOwnership, CloningRejectsAnOccupiedOwnerWithoutChangingEitherObject)
{
	struct chat_message source{};
	char source_content[] = "source";
	source.content = source_content;
	struct chat_message owned NB_CLEANUP(chat_message_cleanup) = {};
	owned.content = strdup("original");
	ASSERT_NE(owned.content, nullptr);
	char *original = owned.content;
	EXPECT_EQ(chat_message_clone(&source, &owned), NB_ERR_EXISTS);
	EXPECT_EQ(owned.content, original);
	EXPECT_STREQ(owned.content, "original");
	EXPECT_STREQ(source.content, "source");
	struct tool_call call{};
	char input[] = "new input";
	call.arguments = input;
	struct tool_call existing NB_CLEANUP(tool_call_cleanup) = {};
	existing.arguments = strdup("old input");
	ASSERT_NE(existing.arguments, nullptr);
	char *old_input = existing.arguments;
	EXPECT_EQ(tool_call_clone(&call, &existing), NB_ERR_EXISTS);
	EXPECT_EQ(existing.arguments, old_input);
	EXPECT_STREQ(existing.arguments, "old input");
}

TEST(TestModelOwnership, StreamTransfersCallsWithoutRetainingAnOwner)
{
	struct chat_response response NB_CLEANUP(chat_response_free) = {};
	struct tool_call *transferred;
	{
		struct llm_stream_ctx stream NB_CLEANUP(llm_stream_cleanup) = {};
		llm_stream_init(&stream, nullptr, nullptr);
		struct tool_call *call = llm_stream_tool_call_ensure_borrow(&stream, 0);
		ASSERT_NE(call, nullptr);
		call->id = strdup("call_1");
		call->arguments = strdup("{\"ok\":true}");
		ASSERT_NE(call->arguments, nullptr);
		transferred = stream.tool_calls;
		ASSERT_EQ(llm_stream_tool_calls_move(&stream, &response), 0);
		EXPECT_EQ(stream.tool_calls, nullptr);
		EXPECT_EQ(stream.tool_call_count, 0);
		EXPECT_EQ(stream.call_storage.len, 0u);
	}
	ASSERT_NE(response.tool_calls, transferred);
	ASSERT_EQ(response.tool_call_count, 1);
	EXPECT_STREQ(response.tool_calls[0].id, "call_1");
	EXPECT_STREQ(response.tool_calls[0].arguments, "{\"ok\":true}");
	chat_response_free(&response);
	EXPECT_EQ(response.tool_calls, nullptr);
	EXPECT_EQ(response.tool_call_count, 0);
}

TEST(TestModelOwnership, StreamPublicationMovesMetadataAndCopiesOnlyNativeArguments)
{
	struct llm_stream_ctx stream NB_CLEANUP(llm_stream_cleanup) = {};
	struct chat_response response NB_CLEANUP(chat_response_free) = {};
	llm_stream_init(&stream, nullptr, nullptr);
	struct tool_call *call = llm_stream_tool_call_ensure_borrow(&stream, 0);
	ASSERT_NE(call, nullptr);
	call->id = strdup("call");
	call->name = strdup("tool");
	call->tool_call_id = strdup("wire-call");
	call->provider_item_id = strdup("item");
	ASSERT_NE(call->id, nullptr);
	ASSERT_NE(call->name, nullptr);
	ASSERT_NE(call->tool_call_id, nullptr);
	ASSERT_NE(call->provider_item_id, nullptr);
	ASSERT_EQ(llm_stream_call_input_append(&stream, call, "argument"), 0);
	char *id = call->id, *name = call->name, *arguments = call->arguments;
	char *wire = call->tool_call_id, *item = call->provider_item_id;
	ASSERT_EQ(llm_stream_tool_calls_move(&stream, &response), 0);
	EXPECT_EQ(response.tool_calls[0].id, id);
	EXPECT_EQ(response.tool_calls[0].name, name);
	EXPECT_EQ(response.tool_calls[0].tool_call_id, wire);
	EXPECT_EQ(response.tool_calls[0].provider_item_id, item);
	EXPECT_NE(response.tool_calls[0].arguments, arguments);
	llm_stream_cleanup(&stream);
	EXPECT_STREQ(response.tool_calls[0].arguments, "argument");
	EXPECT_STREQ(response.tool_calls[0].name, "tool");
}

TEST(TestModelOwnership, StreamRejectsInvalidIndicesWithoutChangingCalls)
{
	struct llm_stream_ctx stream NB_CLEANUP(llm_stream_cleanup) = {};
	llm_stream_init(&stream, nullptr, nullptr);
	EXPECT_EQ(llm_stream_tool_call_ensure_borrow(&stream, -1), nullptr);
	EXPECT_EQ(llm_stream_tool_call_ensure_borrow(&stream, INT_MAX), nullptr);
	EXPECT_EQ(stream.tool_calls, nullptr);
	EXPECT_EQ(stream.tool_call_count, 0);
	EXPECT_EQ(stream.call_storage.len, 0u);
}

TEST(TestModelOwnership, StreamCallMoveReplacesAnExistingDestinationOwner)
{
	struct chat_response response NB_CLEANUP(chat_response_free) = {};
	response.content = strdup("existing content");
	response.tool_calls = static_cast<tool_call *>(calloc(1, sizeof(*response.tool_calls)));
	ASSERT_NE(response.content, nullptr);
	ASSERT_NE(response.tool_calls, nullptr);
	response.tool_call_count = 1;
	response.tool_calls[0].arguments = strdup("old arguments");
	ASSERT_NE(response.tool_calls[0].arguments, nullptr);
	struct llm_stream_ctx stream NB_CLEANUP(llm_stream_cleanup) = {};
	llm_stream_init(&stream, nullptr, nullptr);
	struct tool_call *call = llm_stream_tool_call_ensure_borrow(&stream, 0);
	ASSERT_NE(call, nullptr);
	call->arguments = strdup("new arguments");
	ASSERT_NE(call->arguments, nullptr);
	struct tool_call *incoming = stream.tool_calls;
	ASSERT_EQ(llm_stream_tool_calls_move(&stream, &response), 0);
	EXPECT_NE(response.tool_calls, incoming);
	ASSERT_EQ(response.tool_call_count, 1);
	EXPECT_STREQ(response.tool_calls[0].arguments, "new arguments");
	EXPECT_STREQ(response.content, "existing content");
	EXPECT_EQ(stream.tool_calls, nullptr);
	EXPECT_EQ(stream.tool_call_count, 0);
}

TEST(TestModelOwnership, SchemaCopyDoesNotNormalizeOrConsumeItsObjectSource)
{
	cJSON *source NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(
		"{\"description\":\"source\",\"properties\":{\"value\":{\"type\":\"string\"}}}");
	ASSERT_NE(source, nullptr);
	cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = llm_codec_schema_copy(source);
	ASSERT_NE(copy, nullptr);
	EXPECT_NE(copy, source);
	EXPECT_FALSE(cJSON_HasObjectItem(source, "type"));
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(copy, "type")), "object");
	cJSON_SetValuestring(cJSON_GetObjectItem(source, "description"), "changed");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(copy, "description")), "source");
	cJSON_DeleteOwner(&source);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(copy, "description")), "source");
}

TEST(TestModelOwnership, SchemaCopyOwnsAnIndependentProjectionOfAnArraySource)
{
	cJSON *source NB_CLEANUP(cJSON_DeleteOwner) =
		cJSON_Parse("[{\"name\":\"value\",\"description\":\"source\",\"required\":true}]");
	ASSERT_NE(source, nullptr);
	cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = llm_codec_schema_copy(source);
	ASSERT_NE(copy, nullptr);
	EXPECT_TRUE(cJSON_IsArray(source));
	ASSERT_EQ(cJSON_GetArraySize(source), 1);
	cJSON *source_item = cJSON_GetArrayItem(source, 0);
	cJSON_SetValuestring(cJSON_GetObjectItem(source_item, "name"), "changed");
	cJSON_DeleteOwner(&source);
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
	cJSON *source NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateString("not a schema");
	ASSERT_NE(source, nullptr);
	EXPECT_EQ(llm_codec_schema_copy(source), nullptr);
	EXPECT_TRUE(cJSON_IsString(source));
	EXPECT_STREQ(cJSON_GetStringValue(source), "not a schema");
	EXPECT_EQ(llm_codec_schema_copy(nullptr), nullptr);
}

TEST(TestModelOwnership, ChatBodyCreationRejectsAnOccupiedOwner)
{
	cJSON *value NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateString("original");
	ASSERT_NE(value, nullptr);
	char *body NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(value);
	ASSERT_NE(body, nullptr);
	char *original = body;
	size_t length = 37;
	EXPECT_EQ(llm_chat_body_create(nullptr, "model", nullptr, nullptr, 0, 0, 0, nullptr,
				       nullptr, 0, &body, &length),
		  NB_ERR_EXISTS);
	EXPECT_EQ(body, original);
	EXPECT_EQ(length, 37U);
	EXPECT_STREQ(body, "\"original\"");
}


TEST(TestModelOwnership, ResponsesCompletedInputsReplaceIncompleteDeltaPrefixes)
{
	for (bool custom : {false, true}) {
		SCOPED_TRACE(custom);
		struct model model{};
		strcpy(model.adapter, "openai-responses");
		const struct llm_codec *codec = llm_codec_borrow(&model);
		struct llm_stream_ctx stream NB_CLEANUP(llm_stream_cleanup) = {};
		llm_stream_init(&stream, nullptr, nullptr);
		std::string kind = custom ? "custom_tool_call" : "function_call";
		std::string field = custom ? "input" : "arguments";
		std::string delta_type = custom ? "response.custom_tool_call_input.delta" :
			"response.function_call_arguments.delta";
		std::string done_type = custom ? "response.custom_tool_call_input.done" :
			"response.function_call_arguments.done";
		std::string added = "{\"type\":\"response.output_item.added\","
			"\"item\":{\"id\":\"item\","
			"\"call_id\":\"call\",\"name\":\"probe\",\"type\":\"" + kind + "\"}}";
		ASSERT_EQ(codec->event("message", added.c_str(), &stream), 0);
		std::string delta = "{\"type\":\"" + delta_type +
			"\",\"item_id\":\"item\",\"delta\":\"prefix\"}";
		ASSERT_EQ(codec->event("message", delta.c_str(), &stream), 0);
		std::string done = "{\"type\":\"" + done_type +
			"\",\"item_id\":\"item\",\"" + field + "\":\"complete input\"}";
		ASSERT_EQ(codec->event("message", done.c_str(), &stream), 0);
		ASSERT_EQ(stream.tool_call_count, 1);
		EXPECT_STREQ(stream.tool_calls[0].arguments, "complete input");
		done = "{\"type\":\"response.output_item.done\",\"item\":{\"id\":\"item\","
			"\"call_id\":\"call\",\"name\":\"probe\",\"type\":\"" + kind +
			"\",\"" + field + "\":\"authoritative item input\"}}";
		ASSERT_EQ(codec->event("message", done.c_str(), &stream), 0);
		EXPECT_STREQ(stream.tool_calls[0].arguments, "authoritative item input");
	}
}

TEST(TestModelOwnership, NativeStreamBuffersSurviveGrowthAndPublication)
{
	struct llm_stream_ctx stream NB_CLEANUP(llm_stream_cleanup) = {};
	struct chat_response response NB_CLEANUP(chat_response_free) = {};
	llm_stream_init(&stream, nullptr, nullptr);
	for (int i = 0; i < 40; i++) {
		struct tool_call *call = llm_stream_tool_call_ensure_borrow(&stream, i);
		ASSERT_NE(call, nullptr);
		for (int j = 0; j < 256; j++)
			ASSERT_EQ(llm_stream_call_input_append(&stream, call, "fragment"), 0);
		EXPECT_EQ(strlen(call->arguments), 256u * 8u);
	}
	ASSERT_EQ(llm_stream_tool_calls_move(&stream, &response), 0);
	llm_stream_cleanup(&stream);
	ASSERT_EQ(response.tool_call_count, 40);
	for (int i = 0; i < response.tool_call_count; i++) {
		EXPECT_EQ(strlen(response.tool_calls[i].arguments), 256u * 8u);
		EXPECT_EQ(strncmp(response.tool_calls[i].arguments, "fragmentfragment", 16), 0);
	}
}
