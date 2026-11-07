#include <gtest/gtest.h>
#include <climits>
#include "agent/context.h"
#include "agent/tokenizer.h"
#include "agent/react_request.h"
#include "agent/history.h"
#include "agent/react_tool_worker.h"
#include "util/cleanup.h"
#include <string.h>

static void append_message_owned(struct message_list **head, struct message_list *message)
{
	struct message_list *owned MORPH_CLEANUP(msg_list_clear) = message;
	if (owned)
		EXPECT_EQ(msg_list_append_take(head, &owned), 0);
}

class ContextTest : public ::testing::Test {
protected:
	struct tokenizer *tok;

	void SetUp() override {
		tok = tokenizer_create(NULL, "gpt-4o", 128000);
	}
	void TearDown() override {
		tokenizer_destroy(tok);
	}
};

TEST_F(ContextTest, CreateMessage) {
	struct message_list *m MORPH_CLEANUP(msg_list_clear) =
		msg_list_create("user", "hello world", 3);
	ASSERT_NE(m, nullptr);
	EXPECT_STREQ(m->role, "user");
	EXPECT_STREQ(m->content, "hello world");
	EXPECT_EQ(m->token_count, 3);
}

TEST_F(ContextTest, CreateNullRole) {
	struct message_list *m MORPH_CLEANUP(msg_list_clear) =
		msg_list_create(nullptr, "content", 1);
	ASSERT_NE(m, nullptr);
	EXPECT_STREQ(m->role, "");
}

TEST_F(ContextTest, AppendMessages) {
	struct message_list *head MORPH_CLEANUP(msg_list_clear) = nullptr;
	append_message_owned(&head, msg_list_create("user", "hi", 1));
	append_message_owned(&head, msg_list_create("assistant", "hello", 1));
	append_message_owned(&head, msg_list_create("user", "how are you", 3));
	EXPECT_EQ(msg_list_count(head), 3);
}

TEST_F(ContextTest, AppendNull) {
	struct message_list *head MORPH_CLEANUP(msg_list_clear) = nullptr;
	append_message_owned(&head, nullptr);
	EXPECT_EQ(msg_list_count(head), 0);
}

TEST_F(ContextTest, DestroyNull) {
	EXPECT_NO_FATAL_FAILURE(msg_list_destroy(nullptr));
}

TEST_F(ContextTest, TokenCountWithTokenizer) {
	struct message_list *head MORPH_CLEANUP(msg_list_clear) = nullptr;
	append_message_owned(&head, msg_list_create("user", "hello world", 0));
	append_message_owned(&head, msg_list_create("assistant", "hi there", 0));
	int count = context_token_count(head, tok);
	EXPECT_GT(count, 0);
}

TEST_F(ContextTest, TokenCountWithoutTokenizer) {
	struct message_list *head MORPH_CLEANUP(msg_list_clear) = nullptr;
	append_message_owned(&head, msg_list_create("user", "hello", 5));
	append_message_owned(&head, msg_list_create("assistant", "hi", 2));
	int count = context_token_count(head, nullptr);
	EXPECT_EQ(count, 7);
}

TEST_F(ContextTest, EmptyList) {
	EXPECT_EQ(msg_list_count(nullptr), 0);
	EXPECT_EQ(context_token_count(nullptr, tok), 0);
}

TEST_F(ContextTest, NeedsCompress) {
	struct compress_config cfg = {
		.max_context_tokens = 100,
		.max_history_rounds = 6,
		.summarize_threshold_ratio = 0.8,
		.compress_target_ratio = 0.5,
	};
	struct message_list *head MORPH_CLEANUP(msg_list_clear) = nullptr;
	append_message_owned(&head, msg_list_create("user", "short message", 0));
	EXPECT_EQ(context_needs_compress(head, tok, &cfg), 0);

	char long_text[600];
	int pos = 0;
	for (int i = 0; i < 100 && pos < 598; i++) {
		long_text[pos++] = (char)0xE4;
		long_text[pos++] = (char)0xB8;
		long_text[pos++] = (char)0xAD;
	}
	long_text[pos] = '\0';
	append_message_owned(&head, msg_list_create("user", long_text, 0));
	int count = context_token_count(head, tok);
	EXPECT_GE(count, 80) << "Expected >= 80 tokens for 100 CJK chars, got " << count;
	EXPECT_EQ(context_needs_compress(head, tok, &cfg), 1);
}

TEST_F(ContextTest, NeedsCompressNullCfg) {
	struct message_list *head MORPH_CLEANUP(msg_list_clear) = nullptr;
	append_message_owned(&head, msg_list_create("user", "test", 1));
	EXPECT_EQ(context_needs_compress(head, tok, nullptr), 0);
}

TEST_F(ContextTest, LargeTokenCountsSaturateWithoutDisablingCompression)
{
	struct message_list *head MORPH_CLEANUP(msg_list_clear) = nullptr;
	append_message_owned(&head, msg_list_create("user", "first", INT_MAX));
	append_message_owned(&head, msg_list_create("assistant", "second", INT_MAX));
	EXPECT_EQ(context_token_count(head, tok), INT_MAX);
	struct compress_config cfg = {};
	cfg.max_context_tokens = 128000;
	cfg.summarize_threshold_ratio = .8;
	EXPECT_EQ(context_needs_compress(head, tok, &cfg), 1);
	cfg.max_context_tokens = 0;
	EXPECT_EQ(context_needs_compress(head, tok, &cfg), 0);
}

TEST_F(ContextTest, AppendTakeClearsSourceOnlyOnSuccess)
{
	struct message_list *head MORPH_CLEANUP(msg_list_clear) = nullptr;
	struct message_list *message MORPH_CLEANUP(msg_list_clear) =
		msg_list_create("user", "owned payload", 1);
	ASSERT_NE(message, nullptr);
	auto *original = message;
	EXPECT_EQ(msg_list_append_take(nullptr, &message), -EINVAL);
	EXPECT_EQ(message, original);
	ASSERT_EQ(msg_list_append_take(&head, &message), 0);
	EXPECT_EQ(message, nullptr);
	EXPECT_EQ(head, original);
	EXPECT_STREQ(head->content, "owned payload");
	EXPECT_EQ(msg_list_append_take(&head, &head), -EINVAL);
	EXPECT_EQ(head, original);
}

TEST_F(ContextTest, RequestOwnsIndependentCopiesAndRejectsOccupiedOutput)
{
	struct tool_registry registry MORPH_CLEANUP(tool_registry_cleanup) = {};
	tool_registry_init(nullptr, &registry);
	struct react_context *ctx MORPH_CLEANUP(react_context_clear) =
		react_context_create(&registry, tok, nullptr, nullptr);
	ASSERT_NE(ctx, nullptr);
	struct model model{};
	struct chat_message source MORPH_CLEANUP(chat_message_cleanup) = {};
	struct react_request_messages request MORPH_CLEANUP(react_request_messages_cleanup) = {};
	char prompt[] = "initial system prompt";

	source.role = strdup("assistant");
	source.content = strdup("initial content");
	source.reasoning_content = strdup("initial reasoning");
	source.tool_calls = static_cast<struct tool_call *>(calloc(1, sizeof(*source.tool_calls)));
	ASSERT_NE(source.tool_calls, nullptr);
	source.tool_call_count = 1;
	source.tool_calls[0].arguments = strdup("{\"input\":1}");
	ASSERT_EQ(react_request_messages_create(ctx, &model, prompt, &source, 1, &request), 0);
	ASSERT_EQ(request.messages.nelts, 1U);
	auto *copy = static_cast<struct chat_message *>(request.messages.elts);
	EXPECT_NE(copy->content, source.content);
	EXPECT_NE(copy->tool_calls, source.tool_calls);
	EXPECT_NE(copy->tool_calls[0].arguments, source.tool_calls[0].arguments);
	prompt[0] = 'X';
	chat_message_cleanup(&source);
	EXPECT_STREQ(request.system_prompt, "initial system prompt");
	EXPECT_STREQ(copy->content, "initial content");
	EXPECT_STREQ(copy->reasoning_content, "initial reasoning");
	EXPECT_STREQ(copy->tool_calls[0].arguments, "{\"input\":1}");
	auto *retained = request.system_prompt;
	EXPECT_EQ(react_request_messages_create(ctx, &model, "replacement", nullptr, 0, &request),
		  -EALREADY);
	EXPECT_EQ(request.system_prompt, retained);
}

TEST_F(ContextTest, WorkerRetainsOptionalPayloadsThatWereNotCollected)
{
	struct tool_registry registry MORPH_CLEANUP(tool_registry_cleanup) = {};
	tool_registry_init(nullptr, &registry);
	struct react_context *ctx MORPH_CLEANUP(react_context_clear) =
		react_context_create(&registry, tok, nullptr, nullptr);
	ASSERT_NE(ctx, nullptr);
	auto *worker = react_tool_worker_create(&registry, ctx, "test", "{}", "call", "call",
						nullptr, nullptr);
	ASSERT_NE(worker, nullptr);
	worker->result = strdup("completed");
	worker->data = cJSON_CreateObject();
	worker->ui = cJSON_CreateObject();
	worker->meta = cJSON_CreateObject();
	auto *retained_data = worker->data;
	auto *retained_ui = worker->ui;
	auto *retained_meta = worker->meta;
	char *result MORPH_CLEANUP(morph_charp_cleanup) = nullptr;
	int rc = -EIO;
	react_tool_worker_collect(worker, &rc, &result, nullptr, nullptr, nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(result, "completed");
	EXPECT_EQ(worker->result, nullptr);
	EXPECT_EQ(worker->data, retained_data);
	EXPECT_EQ(worker->ui, retained_ui);
	EXPECT_EQ(worker->meta, retained_meta);
	react_tool_worker_destroy(worker);
}

TEST_F(ContextTest, HistoryCopyFailurePreservesEmptyArrayStorage)
{
	struct model_history_item user{};
	struct model_history_item invalid{};
	strcpy(user.kind, "user_message");
	strcpy(user.role, "user");
	user.content = const_cast<char *>("independent input");
	user.next = &invalid;
	strcpy(invalid.kind, "assistant_tool_calls");
	invalid.payload_json = const_cast<char *>("invalid");
	morph_array_t messages MORPH_CLEANUP(agent_chat_messages_cleanup) = {};
	ASSERT_EQ(morph_array_init(&messages, 4, sizeof(struct chat_message)), 0);
	void *storage = messages.elts;
	EXPECT_EQ(agent_history_messages_copy(nullptr, &user, &messages), MORPH_ERR_PARSE);
	EXPECT_EQ(messages.elts, storage);
	EXPECT_EQ(messages.nelts, 0U);
	user.next = nullptr;
	ASSERT_EQ(agent_history_messages_copy(nullptr, &user, &messages), 0);
	ASSERT_EQ(messages.nelts, 1U);
	storage = messages.elts;
	EXPECT_EQ(agent_history_messages_copy(nullptr, &user, &messages), -EALREADY);
	EXPECT_EQ(messages.elts, storage);
}

TEST_F(ContextTest, MessageAndCheckpointConstructorsPreserveOccupiedOutputs)
{
	struct tool_registry registry MORPH_CLEANUP(tool_registry_cleanup) = {};
	tool_registry_init(nullptr, &registry);
	struct react_context *ctx MORPH_CLEANUP(react_context_clear) =
		react_context_create(&registry, tok, nullptr, nullptr);
	ASSERT_NE(ctx, nullptr);
	morph_array_t messages MORPH_CLEANUP(agent_chat_messages_cleanup) = {};
	ASSERT_EQ(react_messages_create(ctx, &messages, "first"), 0);
	void *storage = messages.elts;
	EXPECT_EQ(react_messages_create(ctx, &messages, "next"), -EALREADY);
	EXPECT_EQ(messages.elts, storage);
	char *owned MORPH_CLEANUP(morph_charp_cleanup) = strdup("retained");
	ASSERT_NE(owned, nullptr);
	char *owner = owned;
	EXPECT_EQ(agent_history_tool_arguments_copy("{}", &owned), -EALREADY);
	int truncated = 7;
	EXPECT_EQ(agent_history_tool_content_copy(ctx, "replacement", &owned, &truncated),
		  -EALREADY);
	EXPECT_EQ(react_checkpoint_create("input", ctx, &owned), -EALREADY);
	EXPECT_EQ(owned, owner);
	EXPECT_STREQ(owned, "retained");
	EXPECT_EQ(truncated, 7);
}
