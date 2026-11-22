#include <gtest/gtest.h>

extern "C" {
#include "mcp/mcp_internal.h"
#include "runtime/mcp_elicitation.h"
#include "runtime/mcp_sampling.h"
#include "agent/tool_runtime.h"
#include "models/llm.h"
#include "models/llm/content.h"
#include "models/llm/chat_codec.h"
#include "models/llm/responses_codec.h"
#include <nb/file.h>
}

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct McpQuestionFixture {
	std::vector<std::vector<std::string>> replies;
	std::vector<std::string> questions;
	std::vector<std::string> modes;
	size_t next = 0;
	int error = 0;
};

static int mcp_fixture_request(const char *, const char *, const cJSON *, const void *,
			       cJSON **result, void *)
{
	*result = cJSON_CreateObject();
	return *result ? 0 : NB_ERR_NOMEM;
}

TEST(McpCapabilitiesTest, CapabilitiesMatchHandlersAndTheNegotiatedRevision)
{
	struct mcp_client client = {};
	cJSON *empty NB_CLEANUP(cJSON_DeleteOwner) =
		mcp_client_capabilities_create(&client, mcp_protocol_borrow(MCP_PROTOCOL_VERSION));
	ASSERT_NE(empty, nullptr);
	EXPECT_EQ(empty->child, nullptr);
	client.handlers.elicitation = mcp_fixture_request;
	client.handlers.sampling = mcp_fixture_request;
	client.handlers.sampling_tools = 1;
	client.handlers.roots = mcp_fixture_request;
	for (const char *version :
	     {"2024-11-05", "2025-06-18", "2025-11-25", MCP_PROTOCOL_VERSION}) {
		cJSON *caps NB_CLEANUP(cJSON_DeleteOwner) =
			mcp_client_capabilities_create(&client, mcp_protocol_borrow(version));
		ASSERT_NE(caps, nullptr);
		EXPECT_TRUE(cJSON_IsObject(cJSON_GetObjectItem(caps, "roots")));
		const cJSON *sampling = cJSON_GetObjectItem(caps, "sampling");
		EXPECT_TRUE(cJSON_IsObject(sampling));
		EXPECT_EQ(cJSON_HasObjectItem(sampling, "tools"),
			  strcmp(version, "2025-11-25") >= 0);
		const cJSON *elicitation = cJSON_GetObjectItem(caps, "elicitation");
		EXPECT_EQ(cJSON_IsObject(elicitation), strcmp(version, "2025-06-18") >= 0);
		EXPECT_EQ(cJSON_HasObjectItem(elicitation, "url"),
			  strcmp(version, "2025-11-25") >= 0);
		EXPECT_FALSE(cJSON_HasObjectItem(caps, "extensions"));
	}
}

static int mcp_fixture_question(const char *question, const char *const *, int, const char *mode,
				int, int, const struct ask_user_options *, char ***answers,
				int *count, void *opaque)
{
	auto *fixture = static_cast<McpQuestionFixture *>(opaque);
	fixture->questions.emplace_back(question);
	fixture->modes.emplace_back(mode);
	if (fixture->error)
		return fixture->error;

	if (fixture->next >= fixture->replies.size())
		return NB_ERR_CANCELLED;

	const auto &reply = fixture->replies[fixture->next++];
	*count = static_cast<int>(reply.size());
	*answers = static_cast<char **>(calloc(reply.size(), sizeof(**answers)));
	if (!*answers && !reply.empty())
		return NB_ERR_NOMEM;

	for (size_t i = 0; i < reply.size(); i++) {
		(*answers)[i] = strdup(reply[i].c_str());
		if (!(*answers)[i])
			return NB_ERR_NOMEM;
	}

	return 0;
}

class McpElicitationTest : public ::testing::Test
{
      protected:
	McpQuestionFixture fixture;
	struct tool_runtime_context context = {};
	cJSON *result = nullptr;

	void SetUp() override
	{
		context.ask_user_fn = mcp_fixture_question;
		context.ask_user_user_data = &fixture;
	}

	void TearDown() override
	{
		cJSON_Delete(result);
	}

	int run(const char *json)
	{
		cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(json);
		return runtime_mcp_elicitation("fixture-server", "elicitation/create", params,
					       &context, &result, nullptr);
	}
};

TEST_F(McpElicitationTest, EmptyApprovalFormRequiresTheUsersExplicitDecision)
{
	fixture.replies = {{"Accept"}};
	ASSERT_EQ(run(R"({"message":"Allow app access?","mode":"form",
		"requestedSchema":{"type":"object","properties":{}}})"),
		  0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result, "action")), "accept");
	EXPECT_TRUE(cJSON_IsObject(cJSON_GetObjectItem(result, "content")));
	ASSERT_EQ(fixture.questions.size(), 1U);
	EXPECT_NE(fixture.questions[0].find("fixture-server"), std::string::npos);
	EXPECT_NE(fixture.questions[0].find("Allow app access?"), std::string::npos);
}

TEST_F(McpElicitationTest, DeclineAndDismissalAreDistinctAndNeverSubmitContent)
{
	for (const char *choice : {"Decline", "Cancel"}) {
		fixture.replies.push_back({choice});
		ASSERT_EQ(run(R"({"message":"Proceed?",
			"requestedSchema":{"type":"object","properties":{}}})"),
			  0);
		EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result, "action")),
			     !strcmp(choice, "Decline") ? "decline" : "cancel");
		EXPECT_EQ(cJSON_GetObjectItem(result, "content"), nullptr);
		cJSON_DeleteOwner(&result);
	}
}

TEST_F(McpElicitationTest, MissingHumanAdapterAndInterruptionCancelTheForm)
{
	context.ask_user_fn = nullptr;
	ASSERT_EQ(run(R"({"message":"Proceed?"})"), 0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result, "action")), "cancel");
	cJSON_DeleteOwner(&result);
	context.ask_user_fn = mcp_fixture_question;
	fixture.error = NB_ERR_CANCELLED;
	ASSERT_EQ(run(R"({"message":"Proceed?",
		"requestedSchema":{"type":"object","properties":{}}})"),
		  0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result, "action")), "cancel");
}

TEST_F(McpElicitationTest, FieldsAreCollectedValidatedAndReviewedBeforeSubmission)
{
	fixture.replies = {{"Enter a value"}, {"Alice"}, {"-1"}, {"42"}, {"Yes"}, {"Accept"}};
	ASSERT_EQ(run(R"({"message":"Confirm your details","requestedSchema":{
		"type":"object","properties":{"name":{"type":"string","minLength":2},
		"age":{"type":"integer","minimum":0},"enabled":{"type":"boolean"}},
		"required":["name","age","enabled"]}})"),
		  0);
	const cJSON *content = cJSON_GetObjectItem(result, "content");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(content, "name")), "Alice");
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(content, "age")), 42);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(content, "enabled")));
	ASSERT_FALSE(fixture.questions.empty());
	EXPECT_NE(fixture.questions.back().find("Review your answers"), std::string::npos);
}

TEST_F(McpElicitationTest, UrlModeKeepsCredentialsOutOfTheResponse)
{
	fixture.replies = {{"Accept"}};
	ASSERT_EQ(run(R"({"message":"Sign in","mode":"url",
		"url":"https://auth.example.invalid/consent"})"),
		  0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result, "action")), "accept");
	EXPECT_EQ(cJSON_GetObjectItem(result, "content"), nullptr);
	ASSERT_EQ(fixture.questions.size(), 1U);
	EXPECT_NE(fixture.questions[0].find("https://auth.example.invalid/consent"),
		  std::string::npos);
}

TEST_F(McpElicitationTest, UnsafeUrlSchemesAreRejectedBeforePrompting)
{
	EXPECT_EQ(run(R"({"message":"Open this","mode":"url",
		"url":"javascript:malicious"})"),
		  NB_ERR_INVAL);
	EXPECT_TRUE(fixture.questions.empty());
}

TEST_F(McpElicitationTest, InvalidSchemaDoesNotTrapTheUserInAnUnanswerableForm)
{
	EXPECT_EQ(run(R"({"message":"Details","requestedSchema":{"type":"object",
		"properties":{"bad":{"type":"object"}},"required":["bad"]}})"),
		  NB_ERR_INVAL);
	EXPECT_TRUE(fixture.questions.empty());
}

TEST_F(McpElicitationTest, DateValidationRejectsInvalidLeapDays)
{
	fixture.replies = {{"Enter a value"}, {"2025-02-29"}, {"2024-02-29"}, {"Accept"}};
	ASSERT_EQ(run(R"({"message":"Birthday","requestedSchema":{"type":"object",
		"properties":{"date":{"type":"string","format":"date"}},
		"required":["date"]}})"),
		  0);
	EXPECT_STREQ(cJSON_GetStringValue(
			     cJSON_GetObjectItem(cJSON_GetObjectItem(result, "content"), "date")),
		     "2024-02-29");
}

TEST_F(McpElicitationTest, TitledEnumsSubmitValuesAndAllowReviewEdits)
{
	fixture.replies = {{"Enter a value"}, {"low"},	{"Edit answers"},
			   {"Enter a value"}, {"high"}, {"Accept"}};
	ASSERT_EQ(run(R"({"message":"Priority","requestedSchema":{"type":"object",
		"properties":{"priority":{"type":"string","oneOf":[
		{"const":"low","title":"Low priority"},{"const":"high","title":"High priority"}]}},
		"required":["priority"]}})"),
		  0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
			     cJSON_GetObjectItem(result, "content"), "priority")),
		     "high");
	EXPECT_NE(fixture.questions[1].find("Low priority"), std::string::npos);
}

TEST_F(McpElicitationTest, MultiSelectEnumsDefaultsAndOmittedFieldsKeepTheirTypes)
{
	fixture.replies = {{"read", "write"},
			   {"Use the default value"},
			   {"Use the default value"},
			   {"Omit this field"},
			   {"Accept"}};
	ASSERT_EQ(run(R"({"message":"Access details","requestedSchema":{"type":"object",
		"properties":{"roles":{"type":"array","items":{"type":"string",
		"enum":["read","write"]},"minItems":1},"name":{"type":"string","default":"Dora"},
		"enabled":{"type":"boolean","default":true},"age":{"type":"number"}},
		"required":["roles","enabled"]}})"),
		  0);
	const cJSON *content = cJSON_GetObjectItem(result, "content");
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(content, "roles")), 2);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(content, "name")), "Dora");
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(content, "enabled")));
	EXPECT_FALSE(cJSON_HasObjectItem(content, "age"));
	ASSERT_FALSE(fixture.modes.empty());
	EXPECT_EQ(fixture.modes[0], "multi");
}

TEST(McpContentTest, ErrorResultsPreserveStructuredDetailsAndFailureState)
{
	cJSON *value NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(R"({"isError":true,
		"error":"Cannot complete operation","structuredContent":{"reason":"fixture"},
		"content":[{"type":"text","text":"Cannot complete operation"}]})");
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	ASSERT_EQ(mcp_result_content_copy(&result, value), 0);
	ASSERT_EQ(tool_result_finalize(&result), 0);
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(result.envelope, "ok")));
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
			     cJSON_GetObjectItem(result.data, "structuredContent"), "reason")),
		     "fixture");
	EXPECT_NE(std::string(result.text.ptr, result.text.len).find("mcp_tool_error"),
		  std::string::npos);
}

TEST(McpContentTest, BinaryImagesBecomePrivateArtifactsWithTheirExactBytes)
{
	cJSON *value NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(R"({"content":[
		{"type":"image","data":"AAECAwQ=","mimeType":"image/png","_meta":{"key":1}},
		{"type":"resource_link","uri":"resource://test","name":"test"}],
		"structuredContent":{"ok":true},"_meta":{"source":"fixture"}})");
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	ASSERT_EQ(mcp_result_content_copy(&result, value), 0);
	ASSERT_EQ(tool_result_finalize(&result), 0);
	ASSERT_EQ(result.artifacts.count, 1);
	nb_buf bytes NB_CLEANUP(nb_buf_cleanup) = {};
	ASSERT_EQ(nb_file_read_all(result.artifacts.items[0].path, &bytes, 0, nullptr), 0);
	const unsigned char expected[] = {0, 1, 2, 3, 4};
	ASSERT_EQ(bytes.len, sizeof(expected));
	EXPECT_EQ(memcmp(bytes.ptr, expected, sizeof(expected)), 0);
	EXPECT_EQ(std::string(result.text.ptr, result.text.len).find("AAECAwQ="),
		  std::string::npos);
	EXPECT_NE(std::string(result.text.ptr, result.text.len).find("resource://test"),
		  std::string::npos);
	std::string path = result.artifacts.items[0].path;
	EXPECT_EQ(nb_file_remove(path.c_str(), nullptr), 0);
	EXPECT_EQ(nb_file_rmdir(path.substr(0, path.rfind('/')).c_str(), nullptr), 0);
}

TEST(McpContentTest, InvalidBinaryIsRejectedWithoutInventingAnAttachment)
{
	cJSON *value NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(R"({"content":[
		{"type":"image","data":"not base64!","mimeType":"image/png"}]})");
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	EXPECT_EQ(mcp_result_content_copy(&result, value), NB_ERR_PROTOCOL);
	EXPECT_EQ(result.artifacts.count, 0);
}

TEST(McpSamplingTest, ModelCodecsKeepImageBytesAndValidToolResultOrdering)
{
	struct model model = {};
	strcpy(model.provider, "openai");
	struct tool_call call = {};
	call.id = const_cast<char *>("image-call");
	call.name = const_cast<char *>("image-tool");
	call.arguments = const_cast<char *>("{}");
	struct chat_message messages[2] = {};
	messages[0].role = const_cast<char *>("assistant");
	messages[0].tool_calls = &call;
	messages[0].tool_call_count = 1;
	messages[1].role = const_cast<char *>("tool");
	messages[1].tool_call_id = call.id;
	cJSON *parts NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(R"([
		{"type":"text","text":"image result"},
		{"type":"image","data":"AAECAwQ=","mimeType":"image/png"}])");
	messages[1].content_parts = parts;
	cJSON *chat NB_CLEANUP(cJSON_DeleteOwner) =
		llm_chat_codec_borrow()->input(&model, nullptr, messages, 2);
	ASSERT_EQ(cJSON_GetArraySize(chat), 3);
	const cJSON *tool = cJSON_GetArrayItem(chat, 1);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(tool, "role")), "tool");
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(tool, "content")), 1);
	const cJSON *media = cJSON_GetArrayItem(chat, 2);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(media, "role")), "user");
	const cJSON *image = cJSON_GetArrayItem(cJSON_GetObjectItem(media, "content"), 0);
	EXPECT_STREQ(cJSON_GetStringValue(
			     cJSON_GetObjectItem(cJSON_GetObjectItem(image, "image_url"), "url")),
		     "data:image/png;base64,AAECAwQ=");
	cJSON *responses NB_CLEANUP(cJSON_DeleteOwner) =
		llm_responses_codec_borrow()->input(&model, nullptr, messages, 2);
	ASSERT_EQ(cJSON_GetArraySize(responses), 2);
	const cJSON *output = cJSON_GetArrayItem(responses, 1);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(output, "type")),
		     "function_call_output");
	image = cJSON_GetArrayItem(cJSON_GetObjectItem(output, "output"), 1);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(image, "type")), "input_image");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(image, "image_url")),
		     "data:image/png;base64,AAECAwQ=");
	struct chat_message clone NB_CLEANUP(chat_message_cleanup) = {};
	ASSERT_EQ(chat_message_clone(&messages[1], &clone), 0);
	EXPECT_NE(clone.content_parts, parts);
	EXPECT_TRUE(cJSON_Compare(clone.content_parts, parts, 1));
}

TEST(McpRootsTest, WorkspaceUrisEncodeSpacesAndUnicodeWithoutExposingOtherFolders)
{
	struct tool_runtime_context context = {};
	context.workdir = "/tmp/mcp fixture/目录";
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	ASSERT_EQ(runtime_mcp_roots("fixture", "roots/list", nullptr, &context, &result, nullptr),
		  0);
	const cJSON *roots = cJSON_GetObjectItem(result, "roots");
	ASSERT_EQ(cJSON_GetArraySize(roots), 1);
	const char *uri =
		cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(roots, 0), "uri"));
	ASSERT_NE(uri, nullptr);
#if defined(__CYGWIN__) || defined(_WIN32)
	EXPECT_EQ(std::string(uri).find("file:///"), 0U);
	EXPECT_NE(std::string(uri).find("/mcp%20fixture/%E7%9B%AE%E5%BD%95"), std::string::npos);
#else
	EXPECT_STREQ(uri, "file:///tmp/mcp%20fixture/%E7%9B%AE%E5%BD%95");
#endif
}

struct McpSamplingFixture {
	int calls = 0;
	int tokens = 0;
	std::string system;
	std::string model;
};

static int mcp_fixture_generate(struct model *self, const char *system,
				struct chat_message *messages, int count, struct tool_desc *, int,
				struct chat_response *response, sse_callback, void *)
{
	auto *fixture = static_cast<McpSamplingFixture *>(self->handle);
	fixture->calls++;
	fixture->tokens = self->max_tokens;
	fixture->system = system;
	fixture->model = self->model_id;
	EXPECT_GE(count, 1);
	EXPECT_NE(messages[0].content_parts, nullptr);
	response->content = strdup("sampled answer");
	return response->content ? 0 : NB_ERR_NOMEM;
}

TEST(McpSamplingTest, GenerationAndDisclosureBothRequireHumanApproval)
{
	for (int deny_stage : {0, 1, 2}) {
		McpSamplingFixture generation;
		McpQuestionFixture question;
		question.replies = {{deny_stage == 1 ? "Decline" : "Accept"},
				    {deny_stage == 2 ? "Decline" : "Accept"}};
		struct model source = {};
		strcpy(source.model_id, "host-selected-model");
		source.max_tokens = 100;
		source.chat_with_tools = mcp_fixture_generate;
		source.handle = &generation;
		struct tool_runtime_context context = {};
		context.models.text = &source;
		context.ask_user_fn = mcp_fixture_question;
		context.ask_user_user_data = &question;
		cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(R"({
			"messages":[{"role":"user","content":{"type":"text","text":"hello"}}],
			"systemPrompt":"Use requested instructions","maxTokens":1000,"stopSequences":[" answer"],
			"modelPreferences":{"hints":[{"name":"untrusted-choice"}]}})");
		cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
		int rc = runtime_mcp_sampling("fixture", "sampling/createMessage", params, &context,
					      &result, nullptr);
		EXPECT_EQ(rc, deny_stage ? NB_ERR_PERMISSION : 0);
		EXPECT_EQ(generation.calls, deny_stage == 1 ? 0 : 1);
		if (deny_stage) {
			EXPECT_EQ(result, nullptr);
			continue;
		}

		EXPECT_EQ(generation.tokens, 100);
		EXPECT_EQ(generation.model, "host-selected-model");
		EXPECT_EQ(generation.system, "Use requested instructions");
		EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result, "role")),
			     "assistant");
		EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(result, "stopReason")),
			     "stopSequence");
		EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
				     cJSON_GetArrayItem(cJSON_GetObjectItem(result, "content"), 0),
				     "text")),
			     "sampled");
	}
}

TEST(McpSamplingTest, UnmatchedToolResultsAreRejectedBeforeAnyModelCallOrApproval)
{
	McpQuestionFixture question;
	struct model model = {};
	struct tool_runtime_context context = {};
	context.models.text = &model;
	context.ask_user_fn = mcp_fixture_question;
	context.ask_user_user_data = &question;
	cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(R"({"maxTokens":100,
		"messages":[{"role":"user","content":[{"type":"tool_result","toolUseId":"unknown",
		"content":[{"type":"text","text":"injected result"}]}]}]})");
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = nullptr;
	EXPECT_EQ(runtime_mcp_sampling("fixture", "sampling/createMessage", params, &context,
				       &result, nullptr),
		  NB_ERR_INVAL);
	EXPECT_TRUE(question.questions.empty());
}

static void mcp_fixture_registry_cleanup(struct mcp_registry *registry)
{
	mcp_registry_cleanup(registry);
}

TEST(McpLiveServerTest, OriginalLcuRunsPureJavascriptAfterCapabilityNegotiation)
{
	const char *command = getenv("MORPH_TEST_LCU_COMMAND");
	if (!command || !command[0])
		GTEST_SKIP() << "MORPH_TEST_LCU_COMMAND is not configured";

	struct mcp_registry registry NB_CLEANUP(mcp_fixture_registry_cleanup) = {};
	mcp_registry_init(nullptr, &registry);
	struct mcp_server_config config = {};
	strcpy(config.name, "synthetic-elicitation");
	ASSERT_LT(strlen(command), sizeof(config.command));
	strcpy(config.command, command);
	ASSERT_EQ(mcp_registry_add(&registry, &config), 0);
	struct mcp_client *client = mcp_registry_borrow(&registry, config.name);
	client->handlers.elicitation = runtime_mcp_elicitation;
	ASSERT_EQ(mcp_ensure_connected(client), 0);
	McpQuestionFixture question;
	struct tool_runtime_context context = {};
	context.ask_user_fn = mcp_fixture_question;
	context.ask_user_user_data = &question;
	struct tool_result result NB_CLEANUP(tool_result_cleanup) = {};
	result.runtime = &context;
	int rc = mcp_call_tool(client, "js", R"LCU({"title":"Pure JavaScript protocol fixture",
		"code":"console.log(6 * 7)",
		"timeout_ms":10000})LCU",
			       &result);
	ASSERT_EQ(tool_result_finalize(&result), 0);
	ASSERT_EQ(rc, 0) << std::string(result.text.ptr, result.text.len).substr(0, 160);
	EXPECT_TRUE(question.questions.empty());
	EXPECT_NE(std::string(result.text.ptr, result.text.len).find("42"), std::string::npos);
}
