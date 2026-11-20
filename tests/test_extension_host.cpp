#include <gtest/gtest.h>
#include "agent_http_test_support.hpp"
#include "runtime_test_support.hpp"

extern "C" {
#include "agent/react.h"
#include "agent/react_tool_worker.h"
#include "agent/tool_runtime.h"
#include "config/config.h"
#include "errors.h"
#include "ext/ext.h"
#include "ext/manifest.h"
#include "models/image_gen.h"
#include "models/llm.h"
#include "models/video_gen.h"
#include "permission/context.h"
#include "runtime/extension_host.h"
#include "sandbox/loader.h"
#include <nb/cleanup.h>
#include <nb/file.h>
#include <nb/time.h>
#include <nb/toml.h>
}

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

class ExtensionHostTest : public ::testing::Test {
protected:
	std::string directory;
	std::string package;
	std::string output;
	std::unique_ptr<struct config> config;
	struct tool_context *permissions = nullptr;
	struct ext extension{};
	struct tool_runtime_context runtime{};
	struct tool_result result{};
	struct nb_log *logger = nullptr;
	std::vector<std::string> logs;
	std::vector<std::string> events;

	void SetUp() override
	{
		char pattern[] = "/tmp/morph-extension-host-XXXXXX";
		ASSERT_NE(mkdtemp(pattern), nullptr);
		directory = std::filesystem::canonical(pattern).string();
		package = directory + "/exts/probe";
		output = directory + "/output";
		std::filesystem::create_directories(package);
		std::filesystem::create_directory(output);
		config = std::make_unique<struct config>();
		std::strcpy(config->models.text.model, "configured-model");
		std::strcpy(config->general.output_dir, "ignored-config-output");
		logger = nb_log_new(NB_LOG_TRACE,
			[](void *opaque, nb_log_level, const char *record, size_t size) {
				auto *self = static_cast<ExtensionHostTest *>(opaque);
				self->logs.emplace_back(record, size);
			}, this);
		ASSERT_NE(logger, nullptr);
		permissions = tool_context_create(logger, directory.c_str(), output.c_str());
		ASSERT_NE(permissions, nullptr);
		SetPolicy(PERMISSION_FULL, PERMISSION_NEVER, PERMISSION_USER);
		ASSERT_EQ(manifest_parse(logger,
			"name='probe'\ntype='so'\nentry='probe.so'\n[config]\nlabel='settings'\n",
			&extension.manifest), NB_OK);
		std::strcpy(extension.path, package.c_str());
		extension.enabled = 1;
		extension.run_v2 = [](const struct ext_context *, const char *args,
				       char **out) -> int {
			*out = strdup(args);
			return *out ? NB_OK : NB_ERR_NOMEM;
		};
		runtime.permission_context = permissions;
		runtime.config = config.get();
		runtime.workdir = directory.c_str();
		runtime.user_id = "user-a";
		runtime.credit_session_id = "session-a";
		runtime.agent_id = "/root/child";
		runtime.agent_depth = 1;
		runtime.turn_id = "turn-a";
		runtime.tool_call_id = "call-a";
		runtime.deadline_ms = nb_time_monotonic_ms() + 300000;
		runtime.http.logger = logger;
		runtime.event_cb = [](const struct morph_event *event, void *opaque) -> int {
			auto *self = static_cast<ExtensionHostTest *>(opaque);
			self->events.emplace_back(event->name);
			EXPECT_STREQ(event->turn_id, self->runtime.turn_id);
			const cJSON *id = cJSON_GetObjectItemCaseSensitive(
				event->data, "tool_call_id");
			EXPECT_STREQ(cJSON_GetStringValue(id), self->runtime.tool_call_id);
			return NB_OK;
		};
		runtime.event_user_data = this;
	}

	void TearDown() override
	{
		tool_result_cleanup(&result);
		ext_unload(&extension);
		tool_context_destroy(permissions);
		nb_log_free(logger);
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
	}

	void SetPolicy(enum permission_access access, enum permission_approval approval,
		       enum permission_reviewer reviewer)
	{
		struct permission_policy policy{access, approval, reviewer};
		ASSERT_EQ(tool_context_set_permission_policy(permissions, &policy), NB_OK);
	}

	int Execute(const char *args = "{}")
	{
		tool_result_cleanup(&result);
		result.runtime = &runtime;
		return runtime_extension_exec(args, &result, &extension);
	}

	std::string Field(const char *name)
	{
		const cJSON *item = cJSON_GetObjectItemCaseSensitive(result.data, name);
		const char *value = cJSON_GetStringValue(item);
		return value ? value : "";
	}

	void LoadProbe(const char *source)
	{
		ext_unload(&extension);
		std::filesystem::copy(source, package,
			std::filesystem::copy_options::recursive |
			std::filesystem::copy_options::overwrite_existing);
		ASSERT_EQ(ext_load(logger, &extension, package.c_str()), NB_OK);
	}
};

TEST_F(ExtensionHostTest, LoadedNativeReceivesCurrentConfigIdentityPathsAndStreams)
{
	LoadProbe(MORPH_TEST_CONTEXT_EXTENSION_DIR);
	ASSERT_NE(extension.run_v2, nullptr);
	EXPECT_EQ(extension.run, nullptr);
	ASSERT_EQ(Execute(), NB_OK);
	EXPECT_EQ(Field("model"), "configured-model");
	EXPECT_EQ(Field("workdir"), directory);
	EXPECT_EQ(Field("output_dir"), output);
	EXPECT_EQ(Field("extension_dir"), package);
	EXPECT_EQ(Field("user_id"), "user-a");
	EXPECT_EQ(Field("session_id"), "session-a");
	EXPECT_EQ(Field("agent_id"), "/root/child");
	EXPECT_EQ(Field("turn_id"), "turn-a");
	EXPECT_EQ(Field("tool_call_id"), "call-a");
	std::string data = Field("data_dir");
	std::string scratch = Field("temp_dir");
	EXPECT_TRUE(std::filesystem::is_directory(data));
	EXPECT_FALSE(std::filesystem::exists(scratch));
	EXPECT_EQ(events.size(), 1u);
	ASSERT_FALSE(logs.empty());
	EXPECT_NE(logs.back().find("context_probe [turn-a/call-a]"), std::string::npos);
	std::strcpy(config->models.text.model, "updated-model");
	runtime.credit_session_id = "session-b";
	runtime.tool_call_id = "call-b";
	ASSERT_EQ(Execute(), NB_OK);
	EXPECT_EQ(Field("model"), "updated-model");
	EXPECT_EQ(Field("session_id"), "session-b");
	EXPECT_EQ(Field("tool_call_id"), "call-b");
	EXPECT_EQ(Field("data_dir"), data);
	EXPECT_NE(Field("temp_dir"), scratch);
	runtime.user_id = "user-b";
	ASSERT_EQ(Execute(), NB_OK);
	EXPECT_NE(Field("data_dir"), data);
}

TEST_F(ExtensionHostTest, LegacyNativeStillLoadsAndRunsWithoutConfig)
{
	LoadProbe(MORPH_TEST_LEGACY_EXTENSION_DIR);
	ASSERT_EQ(extension.run_v2, nullptr);
	ASSERT_NE(extension.run, nullptr);
	runtime.config = nullptr;
	ASSERT_EQ(Execute("{\"text\":\"hello\"}"), NB_OK);
	EXPECT_EQ(Field("result"), "HELLO");
}

TEST_F(ExtensionHostTest, SettingsAreTypedAndContextBorrowsOnlyForTheCall)
{
	extension.run_v2 = [](const struct ext_context *ctx, const char *, char **out) -> int {
		EXPECT_EQ(ctx->abi_version, EXT_CONTEXT_ABI_VERSION);
		EXPECT_EQ(ctx->struct_size, sizeof(*ctx));
		EXPECT_EQ(ctx->host->struct_size, sizeof(*ctx->host));
		EXPECT_EQ(ctx->call.agent_depth, 1);
		EXPECT_EQ(ctx->permissions.access, PERMISSION_FULL);
		EXPECT_EQ(ctx->permissions.approval, PERMISSION_NEVER);
		EXPECT_GT(ctx->call.deadline_ms, nb_time_monotonic_ms());
		const auto *label = nb_toml_table_get_cstr(ctx->settings, "label");
		EXPECT_EQ(nb_toml_type_of(label), NB_TOML_STRING);
		EXPECT_STREQ(nb_toml_string(label).ptr, "settings");
		EXPECT_EQ(ctx->settings, ctx->manifest->settings);
		*out = strdup("{}");
		return *out ? NB_OK : NB_ERR_NOMEM;
	};
	EXPECT_EQ(Execute(), NB_OK);
}

TEST_F(ExtensionHostTest, PublicConfigStringsUseTheEffectivePerCallSnapshot)
{
	std::strcpy(config->models.text.adapter, "deepseek");
	std::strcpy(config->models.text.api_key, "text-key");
	std::strcpy(config->models.vision.api_key_env, "VISION_KEY");
	extension.run_v2 = [](const struct ext_context *ctx, const char *, char **out) -> int {
		EXPECT_TRUE(EXT_HOST_HAS_CALLBACK(ctx->host, get_config_string));
		auto read = ctx->host->get_config_string;
		EXPECT_STREQ(read(ctx->host_data, "model.text.adapter"), "deepseek");
		EXPECT_STREQ(read(ctx->host_data, "model.text.api_key"), "text-key");
		EXPECT_STREQ(read(ctx->host_data, "model.vision.api_key_env"), "VISION_KEY");
		EXPECT_STREQ(read(ctx->host_data, "model.text.api_base"), "");
		EXPECT_STREQ(read(ctx->host_data, "model.image.provider"), "");
		EXPECT_STREQ(read(ctx->host_data, "model.video.model"), "");
		EXPECT_EQ(read(ctx->host_data, nullptr), nullptr);
		EXPECT_EQ(read(ctx->host_data, "model.text.unknown"), nullptr);
		EXPECT_EQ(read(ctx->host_data, "model.unknown.api_key"), nullptr);
		EXPECT_EQ(read(ctx->host_data, "model.text"), nullptr);
		EXPECT_EQ(read(ctx->host_data, "model.text.api_key.trailing"), nullptr);
		*out = strdup((std::string("{\"model\":\"") +
			read(ctx->host_data, "model.text.model") + "\"}").c_str());
		return *out ? NB_OK : NB_ERR_NOMEM;
	};
	ASSERT_EQ(Execute(), NB_OK);
	EXPECT_EQ(Field("model"), "configured-model");
	std::strcpy(config->models.text.model, "new-model");
	ASSERT_EQ(Execute(), NB_OK);
	EXPECT_EQ(Field("model"), "new-model");
}

struct ExtensionModelState {
	struct model *source = nullptr;
	struct config *original_config = nullptr;
	const struct nb_cancel *cancel = nullptr;
	int chats = 0;
	int usages = 0;
	int budgets = 0;
};

TEST_F(ExtensionHostTest, ChatUsesRequestViewAndPreservesUsageBudgetCancellationAndSnapshot)
{
	struct model source{};
	struct nb_cancel cancel{};
	ExtensionModelState state{&source, config.get(), &cancel};
	struct model_usage_sink usage{
		[](const struct model_usage *, void *opaque) {
			static_cast<ExtensionModelState *>(opaque)->usages++;
		}, &state};
	struct model_usage_sink budget{
		[](const struct model_usage *, void *opaque) {
			static_cast<ExtensionModelState *>(opaque)->budgets++;
		}, &state};
	source.usage_sink = &usage;
	source.budget_sink = &budget;
	source.handle = &state;
	source.timeout_seconds = 600;
	source.chat = [](struct model *model, const char *system, const char **messages,
			 int count, const struct model_chat_options *opts,
			 sse_callback callback, void *opaque) {
		auto *state = static_cast<ExtensionModelState *>(model->handle);
		state->chats++;
		EXPECT_NE(model, state->source);
		EXPECT_EQ(model->http.cancel_token, state->cancel);
		EXPECT_STREQ(system, "system");
		EXPECT_EQ(count, 1);
		EXPECT_STREQ(messages[0], "question");
		EXPECT_STREQ(opts->usage_phase, "extension");
		EXPECT_LE(opts->timeout_seconds, 300);
		std::strcpy(state->original_config->models.text.model, "changed-during-call");
		struct model_usage observed{};
		observed.input_tokens = 12;
		model_report_usage(model, &observed);
		int rc = callback("answer", opaque);
		return rc == NB_OK ? 200 : rc;
	};
	runtime.models.text = &source;
	runtime.http.cancel_token = &cancel;
	extension.run_v2 = [](const struct ext_context *ctx, const char *, char **out) -> int {
		const char *messages[] = {"question"};
		int rc = ctx->host->chat(ctx->host_data, "system", messages, 1, nullptr,
					 nullptr, nullptr);
		EXPECT_STREQ(ctx->config->models.text.model, "configured-model");
		EXPECT_STREQ(ctx->host->get_config_string(ctx->host_data, "model.text.model"),
			     "configured-model");
		*out = strdup("{}");
		return *out ? rc : NB_ERR_NOMEM;
	};
	ASSERT_EQ(Execute(), NB_OK);
	EXPECT_EQ(state.chats, 1);
	EXPECT_EQ(state.usages, 1);
	EXPECT_EQ(state.budgets, 1);
	EXPECT_EQ(events.size(), 1u);
	EXPECT_EQ(source.http.cancel_token, nullptr);
	EXPECT_EQ(source.http.interrupt_check, nullptr);
	EXPECT_EQ(source.timeout_seconds, 600);
	EXPECT_STREQ(config->models.text.model, "changed-during-call");
}

TEST_F(ExtensionHostTest, MissingServicesAndInvalidMediaInputsReturnExplicitErrors)
{
	extension.run_v2 = [](const struct ext_context *ctx, const char *, char **out) -> int {
		const char *messages[] = {"question"};
		EXPECT_EQ(ctx->host->chat(ctx->host_data, "", messages, 1, nullptr,
					 nullptr, nullptr), MORPH_ERR_NOT_CONFIGURED);
		struct ext_image_request image{};
		image.prompt = "image";
		image.reference_count = -1;
		struct image_result image_output{};
		EXPECT_EQ(ctx->host->image(ctx->host_data, &image, &image_output), NB_ERR_INVAL);
		image.reference_count = 0;
		EXPECT_EQ(ctx->host->image(ctx->host_data, &image, &image_output),
			  MORPH_ERR_NOT_CONFIGURED);
		struct ext_video_request video{};
		video.prompt = "video";
		video.duration_seconds = 5;
		video.generate_audio = -1;
		struct video_result video_output{};
		EXPECT_EQ(ctx->host->video(ctx->host_data, &video, &video_output),
			  MORPH_ERR_NOT_CONFIGURED);
		video.audio_count = VIDEO_GEN_MAX_REFERENCE_AUDIOS + 1;
		EXPECT_EQ(ctx->host->video(ctx->host_data, &video, &video_output), NB_ERR_INVAL);
		char **answers = nullptr;
		int answer_count = 0;
		EXPECT_EQ(ctx->host->ask_user(ctx->host_data, "question", nullptr, 0, "single",
			0, 0, nullptr, &answers, &answer_count), NB_ERR_UNSUPPORTED);
		*out = strdup("{}");
		return *out ? NB_OK : NB_ERR_NOMEM;
	};
	EXPECT_EQ(Execute(), NB_OK);
}

TEST_F(ExtensionHostTest, TextServiceUsesTheConfiguredHttpClientAndBillsOnce)
{
	AgentHttpServer server;
	server.start();
	std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/v1";
	std::unique_ptr<struct model, decltype(&model_destroy)> model(
		model_llm_create(logger, "openai", "extension-http-model", url.c_str(), "test-key"),
		model_destroy);
	ASSERT_NE(model, nullptr);
	std::vector<struct model_usage> usages;
	struct model_usage_sink sink{
		[](const struct model_usage *usage, void *opaque) {
			static_cast<std::vector<struct model_usage> *>(opaque)->push_back(*usage);
		}, &usages};
	model->usage_sink = &sink;
	runtime.models.text = model.get();
	extension.run_v2 = [](const struct ext_context *ctx, const char *, char **out) -> int {
		const char *messages[] = {"extension question"};
		int rc = ctx->host->chat(ctx->host_data, "extension system", messages, 1,
			nullptr, nullptr, nullptr);
		*out = strdup("{}");
		return *out ? rc : NB_ERR_NOMEM;
	};
	EXPECT_EQ(Execute(), NB_OK);
	auto requests = server.snapshot();
	ASSERT_EQ(requests.size(), 1u);
	EXPECT_NE(requests[0].find("extension-http-model"), std::string::npos);
	EXPECT_NE(requests[0].find("extension question"), std::string::npos);
	ASSERT_EQ(usages.size(), 1u);
	EXPECT_EQ(usages[0].total_tokens, 17);
	EXPECT_STREQ(usages[0].phase, "extension");
	EXPECT_FALSE(events.empty());
	runtime.models.text = nullptr;
	model.reset();
}

TEST_F(ExtensionHostTest, VisionUsesItsOwnModelAndAnAuthorizedReferencePath)
{
	std::string reference = directory + "/reference.png";
	std::ofstream(reference) << "fake image for fake vision client";
	std::strcpy(config->general.log_file, reference.c_str());
	struct model vision{};
	struct model text{};
	ExtensionModelState state;
	struct model_usage_sink usage{
		[](const struct model_usage *, void *opaque) {
			static_cast<ExtensionModelState *>(opaque)->usages++;
		}, &state};
	struct model_usage_sink budget{
		[](const struct model_usage *, void *opaque) {
			static_cast<ExtensionModelState *>(opaque)->budgets++;
		}, &state};
	text.usage_sink = &usage;
	text.budget_sink = &budget;
	vision.handle = &state;
	vision.timeout_seconds = 120;
	vision.chat_with_image = [](struct model *model, const char *, const char *prompt,
		const char *path, const struct model_image_chat_options *opts,
		sse_callback callback, void *opaque) -> int {
		EXPECT_STREQ(prompt, "describe");
		EXPECT_NE(std::string(path).find("reference.png"), std::string::npos);
		EXPECT_EQ(opts->max_dim, 1024);
		EXPECT_EQ(model->timeout_seconds, 120);
		struct model_usage usage{};
		model_report_usage(model, &usage);
		return callback("vision response", opaque);
	};
	runtime.models.text = &text;
	runtime.models.vision = &vision;
	extension.run_v2 = [](const struct ext_context *ctx, const char *, char **out) -> int {
		struct model_image_chat_options options{};
		options.max_dim = 1024;
		int rc = ctx->host->vision(ctx->host_data, "system", "describe",
			ctx->config->general.log_file, &options, nullptr, nullptr);
		*out = strdup("{}");
		return *out ? rc : NB_ERR_NOMEM;
	};
	EXPECT_EQ(Execute(), NB_OK);
	EXPECT_EQ(events.size(), 1u);
	EXPECT_EQ(state.usages, 1);
	EXPECT_EQ(state.budgets, 1);
}

TEST_F(ExtensionHostTest, UserQuestionsUseTheCurrentCallbackAndCancellationToken)
{
	struct nb_cancel cancel{};
	runtime.http.cancel_token = &cancel;
	runtime.ask_user_user_data = &cancel;
	runtime.ask_user_fn = [](const char *question, const char *const *, int,
		const char *, int, int, const struct ask_user_options *options,
		char ***answers, int *count, void *opaque) -> int {
		EXPECT_STREQ(question, "Choose");
		EXPECT_EQ(options->cancel_token, opaque);
		*answers = static_cast<char **>(std::calloc(1, sizeof(**answers)));
		if (!*answers)
			return NB_ERR_NOMEM;

		(*answers)[0] = strdup("yes");
		*count = 1;
		return (*answers)[0] ? NB_OK : NB_ERR_NOMEM;
	};
	extension.run_v2 = [](const struct ext_context *ctx, const char *, char **out) -> int {
		char **answers = nullptr;
		int count = 0;
		const char *choices[] = {"yes", "no"};
		int rc = ctx->host->ask_user(ctx->host_data, "Choose", choices, 2, "single",
			1, 1, nullptr, &answers, &count);
		EXPECT_EQ(count, 1);
		if (answers) {
			EXPECT_STREQ(answers[0], "yes");
			for (int i = 0; i < count; i++)
				std::free(answers[i]);

			std::free(answers);
		}

		*out = strdup("{}");
		return *out ? rc : NB_ERR_NOMEM;
	};
	EXPECT_EQ(Execute(), NB_OK);
}

TEST_F(ExtensionHostTest, CancellationAndDeadlinesAreObservedBeforeAndDuringNativeCalls)
{
	runtime.deadline_ms = nb_time_monotonic_ms() - 1;
	EXPECT_EQ(Execute(), NB_ERR_TIMEOUT);
	runtime.deadline_ms = 0;
	struct nb_cancel cancel{};
	nb_cancel_request(&cancel);
	runtime.http.cancel_token = &cancel;
	EXPECT_EQ(Execute(), NB_ERR_CANCELLED);
	nb_cancel_reset(&cancel);
	runtime.ask_user_user_data = &cancel;
	runtime.ask_user_fn = [](const char *, const char *const *, int,
		const char *, int, int, const struct ask_user_options *, char ***, int *,
		void *opaque) -> int {
		nb_cancel_request(static_cast<struct nb_cancel *>(opaque));
		return NB_ERR_CANCELLED;
	};
	extension.run_v2 = [](const struct ext_context *ctx, const char *, char **out) -> int {
		EXPECT_EQ(ctx->host->check_cancelled(ctx->host_data), NB_OK);
		char **answers = nullptr;
		int count = 0;
		EXPECT_EQ(ctx->host->ask_user(ctx->host_data, "question", nullptr, 0, "single",
			0, 0, nullptr, &answers, &count), NB_ERR_CANCELLED);
		EXPECT_EQ(ctx->host->check_cancelled(ctx->host_data), NB_ERR_CANCELLED);
		EXPECT_EQ(ctx->host->emit_stream(ctx->host_data, "status", "late"),
			  NB_ERR_CANCELLED);
		*out = strdup("{}");
		return *out ? NB_OK : NB_ERR_NOMEM;
	};
	EXPECT_EQ(Execute(), NB_ERR_CANCELLED);
	EXPECT_TRUE(events.empty());
}

TEST_F(ExtensionHostTest, ArtifactsSurviveOutputConversionAndScratchCannotEscapeAsAnArtifact)
{
	std::ofstream(output + "/image.png") << "image";
	extension.run_v2 = [](const struct ext_context *ctx, const char *, char **out) -> int {
		std::string path = std::string(ctx->paths.output_dir) + "/image.png";
		int rc = ctx->host->add_artifact(ctx->host_data, TOOL_ARTIFACT_IMAGE,
			path.c_str(), 640, 480, 0);
		std::string scratch = std::string(ctx->paths.temp_dir) + "/scratch";
		std::ofstream(scratch) << "temporary";
		EXPECT_EQ(ctx->host->add_artifact(ctx->host_data, TOOL_ARTIFACT_FILE,
			scratch.c_str(), 0, 0, 0), NB_ERR_INVAL);
		*out = strdup("{\"text\":\"done\"}");
		return *out ? rc : NB_ERR_NOMEM;
	};
	ASSERT_EQ(Execute(), NB_OK);
	ASSERT_EQ(result.artifacts.count, 1);
	EXPECT_EQ(result.artifacts.items[0].kind, TOOL_ARTIFACT_IMAGE);
	EXPECT_EQ(result.artifacts.items[0].width, 640);
	EXPECT_EQ(result.artifacts.items[0].height, 480);
	EXPECT_STREQ(result.artifacts.items[0].path, (output + "/image.png").c_str());
	EXPECT_EQ(Field("text"), "done");
}

struct ExtensionReviewState {
	int calls = 0;
	enum tool_operation_verdict verdict = TOOL_OP_ALLOW;
	std::string principal;
};

TEST_F(ExtensionHostTest, FullNeedsNoApprovalAndExplicitRequirementsKeepTheirMeaning)
{
	extension.run_v2 = [](const struct ext_context *ctx, const char *, char **out) -> int {
		struct tool_operation op{};
		op.kind = TOOL_OP_TOOL_CALL;
		op.action = "ordinary operation";
		EXPECT_EQ(ctx->host->check_operation(ctx->host_data, &op), NB_OK);
		op.forbidden = 1;
		EXPECT_LT(ctx->host->check_operation(ctx->host_data, &op), NB_OK);
		op.forbidden = 0;
		op.require_user = 1;
		EXPECT_LT(ctx->host->check_operation(ctx->host_data, &op), NB_OK);
		*out = strdup("{}");
		return *out ? NB_OK : NB_ERR_NOMEM;
	};
	EXPECT_EQ(Execute(), NB_OK);
}

TEST_F(ExtensionHostTest, RestrictedNativeExecutionCannotMutateFilesEvenAfterToolApproval)
{
	std::string target = directory + "/unchanged";
	std::ofstream(target) << "original";
	std::strcpy(config->general.log_file, target.c_str());
	extension.run_v2 = [](const struct ext_context *ctx, const char *, char **out) -> int {
		std::ofstream(ctx->config->general.log_file) << "changed";
		*out = strdup("{}");
		return *out ? NB_OK : NB_ERR_NOMEM;
	};
	for (auto access : {PERMISSION_WORKSPACE, PERMISSION_READ_ONLY}) {
		for (auto reviewer : {PERMISSION_USER, PERMISSION_MODEL}) {
			ExtensionReviewState review;
			auto callback = [](const struct tool_operation *op,
				struct permission_review *result, void *opaque) -> int {
				auto *review = static_cast<ExtensionReviewState *>(opaque);
				review->calls++;
				review->principal = op->principal;
				result->verdict = review->verdict;
				return NB_OK;
			};
			SetPolicy(access, PERMISSION_ON_REQUEST, reviewer);
			tool_context_set_review_adapters(permissions, callback, &review,
							 callback, &review);
			EXPECT_EQ(Execute(), NB_ERR_UNSUPPORTED);
			EXPECT_GT(review.calls, 0);
			EXPECT_EQ(review.principal, "probe");
			review.verdict = TOOL_OP_DENY;
			EXPECT_LT(Execute(), NB_OK);
			std::ifstream file(target);
			std::string contents;
			file >> contents;
			EXPECT_EQ(contents, "original");
			EXPECT_FALSE(std::filesystem::exists(directory + "/exts/.data"));
			tool_context_set_review_adapters(permissions, nullptr, nullptr,
							 nullptr, nullptr);
		}
	}
}

TEST_F(ExtensionHostTest, HostOperationsCannotImpersonateAnotherPrincipal)
{
	SetPolicy(PERMISSION_FULL, PERMISSION_ON_REQUEST, PERMISSION_USER);
	ExtensionReviewState review;
	tool_context_set_operation_approval(permissions,
		[](const struct tool_operation *op, void *opaque) {
			auto *review = static_cast<ExtensionReviewState *>(opaque);
			review->calls++;
			review->principal = op->principal;
			return review->verdict;
		}, &review);
	extension.run_v2 = [](const struct ext_context *ctx, const char *, char **out) -> int {
		struct tool_operation operation{};
		operation.kind = TOOL_OP_TOOL_CALL;
		operation.principal = "another-tool";
		operation.tool_name = "another-tool";
		operation.require_user = 1;
		int rc = ctx->host->check_operation(ctx->host_data, &operation);
		*out = strdup("{}");
		return *out ? rc : NB_ERR_NOMEM;
	};
	EXPECT_EQ(Execute(), NB_OK);
	EXPECT_EQ(review.calls, 1);
	EXPECT_EQ(review.principal, "probe");
}

TEST(ExtensionWorkerModels, ChildTextAndMediaBindingsReplaceInheritedParentModels)
{
	struct tool_registry tools{};
	tool_registry_init(nullptr, &tools);
	std::unique_ptr<struct react_context, decltype(&react_context_destroy)> react(
		react_context_create(&tools, nullptr, nullptr, nullptr), react_context_destroy);
	ASSERT_NE(react, nullptr);
	struct model parent{};
	struct model child{};
	struct model vision{};
	struct model image{};
	struct model video{};
	react->llm_model = &child;
	react->tool_runtime.models.text = &parent;
	react->tool_runtime.models.vision = &parent;
	react->tool_models = {nullptr, &vision, &image, &video};
	react->agent_depth = 1;
	std::strcpy(react->agent_id, "/root/child");
	react->workdir = strdup("/child/workspace");
	ASSERT_NE(react->workdir, nullptr);
	std::unique_ptr<struct async_tool_call, decltype(&react_tool_worker_destroy)> call(
		react_tool_worker_create(&tools, react.get(), "probe", "{}", "tool-id",
			"provider-id", nullptr, nullptr), react_tool_worker_destroy);
	ASSERT_NE(call, nullptr);
	EXPECT_EQ(call->runtime.models.text, &child);
	EXPECT_EQ(call->runtime.models.vision, &vision);
	EXPECT_EQ(call->runtime.models.image, &image);
	EXPECT_EQ(call->runtime.models.video, &video);
	EXPECT_STREQ(call->runtime.workdir, "/child/workspace");
	EXPECT_STREQ(call->runtime.agent_id, "/root/child");
	call.reset();
	react.reset();
	tool_registry_cleanup(&tools);
}

TEST_F(ExtensionHostTest, RestrictedExecutionWithUnavailableReviewAdaptersFailsWithoutFiles)
{
	SetPolicy(PERMISSION_WORKSPACE, PERMISSION_ON_REQUEST, PERMISSION_USER);
	EXPECT_LT(Execute(), NB_OK);
	SetPolicy(PERMISSION_WORKSPACE, PERMISSION_ON_REQUEST, PERMISSION_MODEL);
	EXPECT_LT(Execute(), NB_OK);
	SetPolicy(PERMISSION_READ_ONLY, PERMISSION_NEVER, PERMISSION_USER);
	EXPECT_LT(Execute(), NB_OK);
	EXPECT_FALSE(std::filesystem::exists(directory + "/exts/.data"));
}

TEST_F(ExtensionHostTest, FilesystemFailureIsSeparateFromPolicyDenial)
{
	std::filesystem::remove_all(package);
	EXPECT_EQ(Execute(), NB_ERR_NOT_FOUND);
}

TEST(ExtensionContextAbi, RejectsMissingAndInvalidContextBeforeCallingNative)
{
	struct ext extension{};
	extension.enabled = 1;
	extension.run_v2 = [](const struct ext_context *, const char *, char **) -> int {
		ADD_FAILURE() << "invalid context reached native callback";
		return NB_OK;
	};
	char *output = nullptr;
	EXPECT_EQ(ext_run(&extension, "{}", &output), NB_ERR_INVAL);
	struct ext_context context{};
	context.struct_size = sizeof(context);
	EXPECT_EQ(ext_run_with_context(&extension, &context, "{}", &output,
		nullptr, nullptr, nullptr), NB_ERR_INVAL);
	context.abi_version = EXT_CONTEXT_ABI_VERSION;
	context.struct_size--;
	EXPECT_EQ(ext_run_with_context(&extension, &context, "{}", &output,
		nullptr, nullptr, nullptr), NB_ERR_INVAL);
	struct sandbox_config limits{};
	context.struct_size = sizeof(context);
	EXPECT_EQ(ext_run_with_context(&extension, &context, "{}", &output,
		nullptr, nullptr, &limits), NB_ERR_UNSUPPORTED);
	EXPECT_EQ(output, nullptr);
}

TEST(ExtensionManifestSettings, OwnsTypedSettingsAndRejectsInvalidAndOccupiedOwners)
{
	struct ext_manifest manifest NB_CLEANUP(ext_manifest_cleanup) = {};
	ASSERT_EQ(manifest_parse(nullptr, "[config]\nlabel='example'\nretries=3\n",
				&manifest), NB_OK);
	ASSERT_NE(manifest.settings_owner, nullptr);
	EXPECT_EQ(nb_toml_integer(nb_toml_table_get_cstr(manifest.settings, "retries")), 3);
	EXPECT_EQ(manifest_parse(nullptr, "[config]\nlabel='replacement'\n", &manifest),
		  NB_ERR_EXISTS);
	const auto *label = nb_toml_table_get_cstr(manifest.settings, "label");
	EXPECT_STREQ(nb_toml_string(label).ptr, "example");
	ext_manifest_cleanup(&manifest);
	std::memset(&manifest, 0, sizeof(manifest));
	EXPECT_EQ(manifest_parse(nullptr, "config='invalid'\n", &manifest), NB_ERR_INVAL);
	EXPECT_EQ(manifest.settings_owner, nullptr);
}

class ExtensionRuntimeTest : public RuntimeFacadeTest {};

TEST_F(ExtensionRuntimeTest, ModelToolCallLoadsTheNativeContextThroughTheRuntimeFacade)
{
	AgentHttpServer server;
	std::atomic<int> calls{0};
	server.respond = [&calls](const std::string &) {
		if (calls.fetch_add(1) == 0)
			return std::string(
				R"({"tool_calls":[{"index":0,"id":"native-context-call",)"
				R"("type":"function","function":{"name":"context_probe",)"
				R"("arguments":"{}"}}]})");

		return std::string(R"({"content":"Context inspected."})");
	};
	server.start();
	std::string extensions = directory + "/extensions";
	std::string package = extensions + "/context-probe";
	std::filesystem::create_directories(package);
	std::filesystem::copy(MORPH_TEST_CONTEXT_EXTENSION_DIR, package,
		std::filesystem::copy_options::recursive |
		std::filesystem::copy_options::overwrite_existing);
	WriteConfig("[memory]\nenabled=false\n[permissions]\naccess='full'\n"
		"approval='never'\nreviewer='user'\n[ext]\ndir='" + extensions +
		"'\n[model.text]\nprovider='openai'\nmodel='context-runtime-model'\n"
		"api_key='test-key'\napi_base='http://127.0.0.1:" +
		std::to_string(server.port) + "/v1'\n");
	struct runtime_options options{};
	std::string output = directory + "/output";
	options.config_path = config_path.c_str();
	options.output_dir_override = output.c_str();
	options.enable_shell_exts = 1;
	Open(&options);
	ASSERT_NE(instance, nullptr);
	struct Output {
		std::string observation;
		std::string tool_call_id;
	} observed;
	struct runtime_request request{};
	request.model_input = "Inspect the extension context.";
	request.stored_user_input = request.model_input;
	request.user_id = "runtime-user";
	request.turn_id = "runtime-extension-turn";
	request.turn_flags = AGENT_TURN_DEFAULT_FLAGS;
	request.output_cb = [](const struct react_output_event *event, void *opaque) -> int {
		if (event->type == REACT_STEP_OBSERVATION && event->text) {
			auto *output = static_cast<Output *>(opaque);
			output->observation = event->text;
			output->tool_call_id = event->tool_call_id ? event->tool_call_id : "";
		}

		return NB_OK;
	};
	request.output_user_data = &observed;
	struct runtime_result result{};
	ASSERT_EQ(runtime_execute_turn(instance, &request, &result), NB_OK);
	EXPECT_EQ(result.outcome, RUNTIME_OUTCOME_COMPLETED);
	EXPECT_STREQ(result.final_text, "Context inspected.");
	EXPECT_NE(observed.observation.find("context-runtime-model"), std::string::npos);
	EXPECT_NE(observed.observation.find("runtime-user"), std::string::npos);
	EXPECT_NE(observed.observation.find("runtime-extension-turn"), std::string::npos);
	EXPECT_FALSE(observed.tool_call_id.empty());
	EXPECT_NE(observed.observation.find(observed.tool_call_id), std::string::npos);
	EXPECT_EQ(calls.load(), 2);
	runtime_close(instance);
	instance = nullptr;
}
