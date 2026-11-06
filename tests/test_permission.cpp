#include "runtime_test_support.hpp"
#include "model_metadata_test_support.hpp"

extern "C" {
#include "permission/context.h"
#include "runtime/approval.h"
#include "runtime/runtime_internal.h"
#include "agent/tools/apply_patch.h"
#include "agent/tools/exec_tool.h"
#include "config/edit.h"
#include "sandbox/permission.h"
#include "ext/ext.h"
}

#include <sys/stat.h>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace {
struct Reviews {
	int calls = 0;
	int rc = 0;
	int defer = 0;
	enum tool_operation_verdict verdict = TOOL_OP_ALLOW;
};

int review(const struct tool_operation *, struct permission_review *out, void *opaque)
{
	auto *state = static_cast<Reviews *>(opaque);
	state->calls++;
	out->verdict = state->verdict;
	out->defer = state->defer;
	std::snprintf(out->reason, sizeof(out->reason), "Test decision");
	return state->rc;
}

enum tool_operation_verdict approve(const struct tool_operation *, void *opaque)
{
	auto *calls = static_cast<int *>(opaque);
	(*calls)++;
	return TOOL_OP_ALLOW;
}
}

TEST(Permission, EveryPolicyCombinationHasDeterministicRequirements)
{
	struct tool_operation operation{};
	for (int a = PERMISSION_READ_ONLY; a <= PERMISSION_FULL; a++) {
		for (int p = PERMISSION_ON_REQUEST; p <= PERMISSION_NEVER; p++) {
			for (int r = PERMISSION_USER; r <= PERMISSION_MODEL; r++) {
				struct permission_policy policy{static_cast<permission_access>(a),
					static_cast<permission_approval>(p), static_cast<permission_reviewer>(r)};
				ASSERT_EQ(permission_policy_validate(&policy), 0);
				EXPECT_EQ(permission_check(&policy, &operation, 1), PERMISSION_ALLOW);
				EXPECT_EQ(permission_check(&policy, &operation, 0), a == PERMISSION_FULL ?
					PERMISSION_ALLOW : p == PERMISSION_NEVER ? PERMISSION_DENY : PERMISSION_REVIEW);
				operation.require_user = 1;
				EXPECT_EQ(permission_check(&policy, &operation, 1), p == PERMISSION_NEVER ?
					PERMISSION_DENY : PERMISSION_REQUIRE_USER);
				operation.forbidden = 1;
				EXPECT_EQ(permission_check(&policy, &operation, 1), PERMISSION_DENY);
				operation = {};
			}
		}
	}
}

TEST(Permission, PresetsAndCustomPoliciesRoundTripWithoutDuplicateModeState)
{
	for (const char *name : {"ask", "auto", "full", "read_only"}) {
		struct permission_policy policy{}, parsed{};
		ASSERT_EQ(runtime_permission_preset(name, &policy), 0);
		EXPECT_STREQ(runtime_permission_preset_name(&policy), name);
		ASSERT_EQ(permission_policy_parse(permission_access_name(policy.access),
			permission_approval_name(policy.approval), permission_reviewer_name(policy.reviewer), &parsed), 0);
		EXPECT_EQ(parsed.access, policy.access);
	}
	struct permission_policy policy{};
	ASSERT_EQ(permission_policy_parse("read_only", "never", "model", &policy), 0);
	EXPECT_STREQ(runtime_permission_preset_name(&policy), "custom");
	EXPECT_EQ(runtime_permission_preset("typo", &policy), -EINVAL);
	EXPECT_EQ(permission_policy_parse("invalid", "never", "user", &policy), -EINVAL);
	EXPECT_EQ(permission_policy_validate(nullptr), -EINVAL);
}

TEST(Permission, AutomaticReviewAndMandatoryHumanShareOneRouter)
{
	struct permission_context context;
	ASSERT_EQ(permission_context_init(&context), 0);
	Reviews model, user;
	context.policy.reviewer = PERMISSION_MODEL;
	context.model_review = review;
	context.model_data = &model;
	context.user_review = review;
	context.user_data = &user;
	struct tool_operation operation{};
	enum tool_operation_verdict verdict;
	EXPECT_EQ(permission_authorize(&context, &operation, PERMISSION_REVIEW, &verdict), 0);
	EXPECT_EQ(model.calls, 1);
	EXPECT_EQ(user.calls, 0);
	EXPECT_EQ(permission_authorize(&context, &operation, PERMISSION_REQUIRE_USER, &verdict), 0);
	EXPECT_EQ(user.calls, 1);
	EXPECT_EQ(model.calls, 1);
	model.defer = 1;
	EXPECT_EQ(permission_authorize(&context, &operation, PERMISSION_REVIEW, &verdict), 0);
	EXPECT_EQ(user.calls, 2);
	model.defer = 0;
	model.rc = -ETIMEDOUT;
	EXPECT_EQ(permission_authorize(&context, &operation, PERMISSION_REVIEW, &verdict), 0);
	EXPECT_EQ(user.calls, 3);
	model.rc = -ECANCELED;
	EXPECT_EQ(permission_authorize(&context, &operation, PERMISSION_REVIEW, &verdict), -ECANCELED);
	EXPECT_EQ(user.calls, 3);
	permission_context_cleanup(&context);
}

TEST(Permission, MissingApprovalAndNeverFailClosed)
{
	struct permission_context context;
	ASSERT_EQ(permission_context_init(&context), 0);
	struct tool_operation operation{};
	enum tool_operation_verdict verdict;
	EXPECT_EQ(permission_authorize(&context, &operation, PERMISSION_REVIEW, &verdict), -EPERM);
	context.policy.approval = PERMISSION_NEVER;
	EXPECT_EQ(permission_authorize(&context, &operation, PERMISSION_REVIEW, &verdict), -EACCES);
	EXPECT_EQ(permission_authorize(&context, &operation, PERMISSION_ALLOW, &verdict), 0);
	EXPECT_EQ(verdict, TOOL_OP_ALLOW);
	permission_context_cleanup(&context);
}

TEST(Permission, ThreeModelDenialsRequireHumanAndCannotCreatePersistentAuthority)
{
	struct permission_context context;
	ASSERT_EQ(permission_context_init(&context), 0);
	Reviews model, user;
	model.verdict = TOOL_OP_DENY;
	context.policy.reviewer = PERMISSION_MODEL;
	context.model_review = review;
	context.model_data = &model;
	context.user_review = review;
	context.user_data = &user;
	struct tool_operation operation{};
	enum tool_operation_verdict verdict;
	for (int i = 0; i < 3; i++)
		EXPECT_EQ(permission_authorize(&context, &operation, PERMISSION_REVIEW, &verdict), -EACCES);
	EXPECT_EQ(permission_authorize(&context, &operation, PERMISSION_REVIEW, &verdict), 0);
	EXPECT_EQ(model.calls, 3);
	EXPECT_EQ(user.calls, 1);
	model.verdict = TOOL_OP_ALWAYS;
	EXPECT_EQ(permission_authorize(&context, &operation, PERMISSION_REVIEW, &verdict), -EPROTO);
	permission_context_cleanup(&context);
}

TEST(Permission, ReviewerJsonRejectsUnknownOrPartialOutput)
{
	struct permission_review result;
	for (const char *verdict : {"allow", "deny", "defer"}) {
		std::string json = std::string("{\"verdict\":\"") + verdict + "\",\"reason\":\"Scoped action\"}";
		EXPECT_EQ(runtime_approval_parse(json.c_str(), &result), 0);
	}
	for (const char *json : {"{}", "null", "[]", "{\"verdict\":\"allow\"}",
		"{\"verdict\":\"always\",\"reason\":\"Invalid\"}",
		"{\"verdict\":\"allow\",\"reason\":\"\"}",
		"{\"verdict\":\"allow\",\"reason\":\"x\",\"scope\":\"session\"}",
		"{\"verdict\":\"allow\",\"reason\":\"x\"} garbage"})
		EXPECT_EQ(runtime_approval_parse(json, &result), -EPROTO) << json;
}

TEST_F(RuntimeFacadeTest, PermissionPolicyPersistsThreeDimensionsAndRetainsOtherConfig)
{
	WriteConfig("# preserve me\n[general]\nauto_recap=false\n[permissions]\naccess='workspace'\n");
	runtime_options options{};
	options.config_path = config_path.c_str();
	Open(&options);
	struct permission_policy policy{};
	ASSERT_EQ(runtime_permission_preset("auto", &policy), 0);
	ASSERT_EQ(runtime_permission_set_policy(instance, &policy), 0);
	struct permission_policy current{};
	ASSERT_EQ(runtime_permission_get_policy(instance, &current), 0);
	EXPECT_EQ(current.reviewer, PERMISSION_MODEL);
	std::ifstream file(config_path);
	std::string text((std::istreambuf_iterator<char>(file)), {});
	EXPECT_NE(text.find("# preserve me"), std::string::npos);
	struct config loaded{};
	ASSERT_EQ(config_load(nullptr, &loaded, config_path.c_str()), 0);
	EXPECT_EQ(loaded.permissions.reviewer, PERMISSION_MODEL);
	EXPECT_EQ(loaded.general.auto_recap, 0);
	EXPECT_EQ(config_validate_text("[permissions]\nreviewer='unknown'\n", nullptr), MORPH_ERR_CONFIG);
}

TEST_F(RuntimeFacadeTest, PermissionPolicyBusyAndSaveFailuresPreserveActivePolicy)
{
	WriteConfig("[permissions]\naccess='workspace'\n");
	runtime_options options{};
	options.config_path = config_path.c_str();
	Open(&options);
	struct permission_policy policy{};
	ASSERT_EQ(runtime_permission_preset("full", &policy), 0);
	pthread_mutex_lock(&instance->context.execution_lock);
	EXPECT_EQ(runtime_permission_set_policy(instance, &policy), -EBUSY);
	pthread_mutex_unlock(&instance->context.execution_lock);
	std::filesystem::remove(config_path);
	EXPECT_NE(runtime_permission_set_policy(instance, &policy), 0);
	EXPECT_EQ(instance->context.tctx->permissions.policy.access, PERMISSION_WORKSPACE);
}

TEST_F(RuntimeFacadeTest, ReadOnlyPatchDenialAndLaterInvalidTargetLeaveFilesUntouched)
{
	auto *context = tool_context_create(nullptr, directory.c_str(), directory.c_str());
	ASSERT_NE(context, nullptr);
	struct tool_registry registry{};
	tool_registry_init(nullptr, &registry);
	ASSERT_EQ(apply_patch_init(&registry, context), 0);
	struct permission_policy policy{};
	ASSERT_EQ(runtime_permission_preset("read_only", &policy), 0);
	ASSERT_EQ(tool_context_set_permission_policy(context, &policy), 0);
	struct tool_result result{};
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&registry, "apply_patch",
		"*** Begin Patch\n*** Add File: first.txt\n+hello\n*** End Patch", &result), -EPERM);
	EXPECT_FALSE(std::filesystem::exists(directory + "/first.txt"));
	int approvals = 0;
	tool_context_set_operation_approval(context, approve, &approvals);
	EXPECT_EQ(tool_exec(&registry, "apply_patch",
		"*** Begin Patch\n*** Add File: first.txt\n+hello\n*** Add File: ../escape.txt\n+bad\n*** End Patch", &result), -EINVAL);
	EXPECT_FALSE(std::filesystem::exists(directory + "/first.txt"));
	EXPECT_EQ(approvals, 0);
	tool_context_set_operation_approval(context,
		[](const struct tool_operation *operation, void *opaque) {
			++*static_cast<int *>(opaque);
			return std::string(operation->target).find("second.txt") == std::string::npos ?
				TOOL_OP_ALLOW : TOOL_OP_DENY;
		}, &approvals);
	EXPECT_EQ(tool_exec(&registry, "apply_patch",
		"*** Begin Patch\n*** Add File: first.txt\n+hello\n*** Add File: second.txt\n+bad\n*** End Patch",
		&result), -EACCES);
	EXPECT_FALSE(std::filesystem::exists(directory + "/first.txt"));
	EXPECT_FALSE(std::filesystem::exists(directory + "/second.txt"));
	EXPECT_EQ(approvals, 2);
	tool_context_set_operation_approval(context, approve, &approvals);
	EXPECT_EQ(tool_exec(&registry, "apply_patch",
		"*** Begin Patch\n*** Add File: first.txt\n+hello\n*** End Patch", &result), 0);
	EXPECT_TRUE(std::filesystem::exists(directory + "/first.txt"));
	EXPECT_GT(approvals, 0);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(context);
}

using PermissionModelTest = RuntimeMetadataTest;

TEST_F(PermissionModelTest, IndependentModelReviewsExactActionWithoutTools)
{
	answer = R"({"verdict":"allow","reason":"Scoped verification"})";
	Start();
	ASSERT_EQ(runtime_approval_init(instance), 0);
	auto *context = instance->context.tctx;
	context->permissions.policy.reviewer = PERMISSION_MODEL;
	std::snprintf(instance->context.approval_objective,
		sizeof(instance->context.approval_objective), "Verify the export");
	struct tool_operation operation{};
	operation.kind = TOOL_OP_TOOL_CALL;
	operation.tool_name = "export";
	operation.action = "exact-action-marker";
	operation.target = "target-marker";
	int rc = -1;
	std::thread worker([&] { rc = tool_context_check_operation(context, &operation); });
	bool requested = WaitRequest();
	if (requested) {
		cJSON *body = cJSON_Parse(request_body.c_str());
		EXPECT_NE(body, nullptr);
		EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(body, "tools")), 0);
		EXPECT_NE(request_body.find("exact-action-marker"), std::string::npos);
		EXPECT_NE(request_body.find("target-marker"), std::string::npos);
		EXPECT_NE(request_body.find("Verify the export"), std::string::npos);
		cJSON_Delete(body);
	}
	Release();
	worker.join();
	ASSERT_TRUE(requested);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(context->scoped_grants.nelts, 0u);
	EXPECT_EQ(context->persistent_grants.nelts, 0u);
	EXPECT_STREQ(instance->context.llm->model_id, "title-test");
}

TEST_F(PermissionModelTest, InvalidModelResponseFallsBackToHumanExactlyOnce)
{
	answer = "unstructured approval";
	Start();
	auto *context = instance->context.tctx;
	ASSERT_EQ(runtime_approval_init(instance), 0);
	context->permissions.policy.reviewer = PERMISSION_MODEL;
	int calls = 0;
	tool_context_set_operation_approval(context, approve, &calls);
	struct tool_operation operation{};
	operation.kind = TOOL_OP_NETWORK;
	operation.target = "example.test";
	int rc = -1;
	std::thread worker([&] { rc = tool_context_check_operation(context, &operation); });
	bool requested = WaitRequest();
	Release();
	worker.join();
	ASSERT_TRUE(requested);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(calls, 1);
	auto *source = instance->context.llm;
	instance->context.llm = nullptr;
	EXPECT_EQ(tool_context_check_operation(context, &operation), 0);
	EXPECT_EQ(calls, 2);
	instance->context.llm = source;
}

TEST_F(RuntimeFacadeTest, ReadOnlyExecCannotWriteProjectEvenWhenItIsInTmp)
{
	auto *context = tool_context_create(nullptr, directory.c_str(), directory.c_str());
	ASSERT_NE(context, nullptr);
	struct permission_policy policy{};
	ASSERT_EQ(runtime_permission_preset("read_only", &policy), 0);
	ASSERT_EQ(tool_context_set_permission_policy(context, &policy), 0);
	struct tool_registry registry{};
	tool_registry_init(nullptr, &registry);
	ASSERT_EQ(exec_tool_init(&registry, context, nullptr), 0);
	struct tool_result result{};
	tool_result_init(&result);
	std::string args = "{\"command\":\"printf forbidden > blocked.txt\",\"workdir\":\"" +
		directory + "\",\"yield_time_ms\":1000}";
	ASSERT_EQ(tool_exec(&registry, "exec", args.c_str(), &result), 0);
	EXPECT_NE(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "exit_code")), 0);
	EXPECT_FALSE(std::filesystem::exists(directory + "/blocked.txt"));
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(context);
}

TEST_F(RuntimeFacadeTest, OpaqueToolsAndChildRegistriesShareAuthority)
{
	auto *context = tool_context_create(nullptr, directory.c_str(), directory.c_str());
	ASSERT_NE(context, nullptr);
	struct tool_registry registry{}, child{};
	tool_registry_init(nullptr, &registry);
	tool_registry_init(nullptr, &child);
	registry.permission_context = context;
	int executions = 0;
	struct tool_spec spec{};
	spec.name = "opaque";
	spec.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.user_data = &executions;
	spec.exec = [](const char *, struct tool_result *result, void *opaque) {
		++*static_cast<int *>(opaque);
		return tool_result_success_text(result, "executed");
	};
	ASSERT_EQ(tool_register(&registry, &spec), 0);
	ASSERT_EQ(tool_registry_copy(&child, &registry), 0);
	EXPECT_EQ(child.permission_context, context);
	struct tool_result result{};
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&child, "opaque", "{}", &result), -EPERM);
	EXPECT_EQ(executions, 0);
	int approvals = 0;
	tool_context_set_operation_approval(context, approve, &approvals);
	EXPECT_EQ(tool_exec(&child, "opaque", "{}", &result), 0);
	EXPECT_EQ(approvals, 1);
	EXPECT_EQ(executions, 1);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&child);
	tool_registry_cleanup(&registry);
	tool_context_destroy(context);
}

TEST_F(RuntimeFacadeTest, ReadOnlyDoesNotInheritProfileOrPersistentWriteAuthority)
{
	auto *context = tool_context_create(nullptr, directory.c_str(), directory.c_str());
	ASSERT_NE(context, nullptr);
	ASSERT_EQ(tool_context_add_exec_profile_path(context, TOOL_PATH_WRITE, directory.c_str()), 0);
	struct permission_policy policy{PERMISSION_READ_ONLY, PERMISSION_NEVER, PERMISSION_USER};
	ASSERT_EQ(tool_context_set_permission_policy(context, &policy), 0);
	struct tool_directory_access_request request{};
	request.access = TOOL_PATH_ACCESS_WRITE;
	request.principal = "*";
	request.command = "Write project files";
	request.path = directory.c_str();
	EXPECT_EQ(tool_context_request_directory_access(context, &request, nullptr, 0), -EACCES);
	EXPECT_EQ(tool_context_authorize_path(context, TOOL_PATH_WRITE,
		(directory + "/blocked").c_str(), nullptr, 0), -EACCES);
	tool_context_destroy(context);
}

TEST_F(RuntimeFacadeTest, ExecHostRequestInWorkspaceCannotBypassApproval)
{
	auto *context = tool_context_create(nullptr, directory.c_str(), directory.c_str());
	ASSERT_NE(context, nullptr);
	struct tool_registry registry{};
	tool_registry_init(nullptr, &registry);
	ASSERT_EQ(exec_tool_init(&registry, context, nullptr), 0);
	struct tool_result result{};
	tool_result_init(&result);
	EXPECT_EQ(tool_exec(&registry, "exec",
		"{\"command\":\"printf forbidden\",\"host\":true}", &result), -EPERM);
	EXPECT_EQ(tool_exec(&registry, "exec",
		"{\"command\":\"printf forbidden\",\"network\":true}", &result), -EPERM);
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(context);
}

TEST(Permission, SandboxIntersectionCannotExceedManifestOrApplication)
{
	struct arena *arena = arena_create(0);
	ASSERT_NE(arena, nullptr);
	char manifest_root[] = "/workspace/project";
	char parent_root[] = "/workspace";
	char sibling[] = "/other";
	char *manifest[] = {manifest_root, sibling};
	char *parent[] = {parent_root};
	struct sandbox_config requested{}, ceiling{};
	requested.write_paths = manifest;
	requested.write_paths_count = 2;
	requested.delete_paths = manifest;
	requested.delete_paths_count = 2;
	requested.network_access = 1;
	requested.allow_temp = 1;
	ceiling.write_paths = parent;
	ceiling.write_paths_count = 1;
	ASSERT_EQ(sandbox_permission_intersect(&requested, &ceiling, arena), 0);
	ASSERT_EQ(requested.write_paths_count, 1);
	EXPECT_STREQ(requested.write_paths[0], manifest_root);
	EXPECT_EQ(requested.delete_paths_count, 0);
	EXPECT_EQ(requested.network_access, 0);
	EXPECT_EQ(requested.allow_temp, 0);
	arena_destroy(arena);
}

TEST_F(RuntimeFacadeTest, LiveBackgroundProcessBlocksPolicyChangeUntilStopped)
{
	WriteConfig("[permissions]\naccess='workspace'\n");
	runtime_options options{};
	options.config_path = config_path.c_str();
	options.enable_exec = 1;
	Open(&options);
	ASSERT_EQ(runtime_session_create_and_select(instance, "permissions-job", nullptr), 0);
	struct tool_runtime_context call{};
	call.memory_session_id = instance->context.current_session.id;
	struct tool_result result{};
	tool_result_init(&result);
	result.runtime = &call;
	ASSERT_EQ(tool_exec(&instance->context.tools, "exec",
		"{\"command\":\"sleep 30\",\"service\":true,\"yield_time_ms\":1}", &result), 0);
	const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "session_id"));
	ASSERT_NE(id, nullptr);
	std::string process_id = id;
	struct permission_policy policy{PERMISSION_READ_ONLY, PERMISSION_ON_REQUEST, PERMISSION_USER};
	EXPECT_EQ(runtime_permission_set_policy(instance, &policy), -EBUSY);
	ASSERT_EQ(runtime_process_stop(instance, process_id.c_str()), 0);
	int rc = -EBUSY;
	for (int i = 0; i < 100 && rc == -EBUSY; i++) {
		rc = runtime_permission_set_policy(instance, &policy);
		if (rc == -EBUSY)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT_EQ(rc, 0);
	tool_result_cleanup(&result);
}

TEST(Permission, ConcurrentReviewsAreSerializedAndKeepTheirOwnReasons)
{
	struct permission_context context;
	ASSERT_EQ(permission_context_init(&context), 0);
	std::atomic<int> active{0};
	context.user_data = &active;
	context.user_review = [](const struct tool_operation *operation,
		struct permission_review *out, void *opaque) {
		auto *running = static_cast<std::atomic<int> *>(opaque);
		EXPECT_EQ(running->fetch_add(1), 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		std::snprintf(out->reason, sizeof(out->reason), "%s", operation->action);
		out->verdict = TOOL_OP_DENY;
		EXPECT_EQ(running->fetch_sub(1), 1);
		return 0;
	};
	std::vector<std::thread> workers;
	for (int i = 0; i < 8; i++) {
		workers.emplace_back([&, i] {
			std::string action = "action-" + std::to_string(i);
			struct permission_review decision{};
			struct tool_operation operation{};
			operation.action = action.c_str();
			operation.review_out = &decision;
			enum tool_operation_verdict verdict;
			EXPECT_EQ(permission_authorize(&context, &operation,
				PERMISSION_REVIEW, &verdict), -EACCES);
			EXPECT_STREQ(decision.reason, action.c_str());
		});
	}
	for (auto &worker : workers)
		worker.join();
	context.user_review = [](const struct tool_operation *, struct permission_review *out,
		void *) {
		std::memset(out->reason, 'x', sizeof(out->reason));
		out->verdict = TOOL_OP_DENY;
		return 0;
	};
	struct permission_review decision{};
	struct tool_operation operation{};
	operation.review_out = &decision;
	enum tool_operation_verdict verdict;
	EXPECT_EQ(permission_authorize(&context, &operation, PERMISSION_REVIEW, &verdict), -EACCES);
	EXPECT_EQ(decision.reason[sizeof(decision.reason) - 1], '\0');
	permission_context_cleanup(&context);
}

TEST_F(RuntimeFacadeTest, ToolCallGrantsRespectScopeMandatoryHumanAndPolicySwitches)
{
	auto *context = tool_context_create(nullptr, directory.c_str(), directory.c_str());
	ASSERT_NE(context, nullptr);
	Reviews user;
	user.verdict = TOOL_OP_SESSION;
	context->permissions.user_review = review;
	context->permissions.user_data = &user;
	struct tool_operation operation{};
	operation.kind = TOOL_OP_TOOL_CALL;
	operation.tool_name = "remote";
	EXPECT_EQ(tool_context_check_operation(context, &operation), 0);
	EXPECT_EQ(tool_context_check_operation(context, &operation), 0);
	EXPECT_EQ(user.calls, 1);
	operation.network = 1;
	EXPECT_EQ(tool_context_check_operation(context, &operation), 0);
	EXPECT_EQ(user.calls, 2);
	operation.require_user = 1;
	EXPECT_EQ(tool_context_check_operation(context, &operation), 0);
	EXPECT_EQ(user.calls, 3);
	operation.forbidden = 1;
	EXPECT_EQ(tool_context_check_operation(context, &operation), -EACCES);
	EXPECT_EQ(user.calls, 3);
	operation = {};
	operation.kind = TOOL_OP_TOOL_CALL;
	operation.tool_name = "remote";
	tool_context_add_read_allowed_dir(context, "/usr");
	EXPECT_EQ(tool_context_authorize_path(context, TOOL_PATH_READ, "/usr", nullptr, 0), 0);
	struct permission_policy policy{PERMISSION_READ_ONLY, PERMISSION_NEVER, PERMISSION_USER};
	ASSERT_EQ(tool_context_set_permission_policy(context, &policy), 0);
	EXPECT_EQ(tool_context_check_operation(context, &operation), -EACCES);
	EXPECT_EQ(tool_context_authorize_path(context, TOOL_PATH_READ, "/usr", nullptr, 0), -EACCES);
	tool_context_destroy(context);
}

TEST_F(RuntimeFacadeTest, RetainedHostProcessInputRequiresAnotherApproval)
{
	for (bool host : {true, false}) {
		auto *context = tool_context_create(nullptr, directory.c_str(), directory.c_str());
		ASSERT_NE(context, nullptr);
		Reviews user;
		context->permissions.user_review = review;
		context->permissions.user_data = &user;
		struct tool_registry registry{};
		tool_registry_init(nullptr, &registry);
		ASSERT_EQ(exec_tool_init(&registry, context, nullptr), 0);
		struct tool_result result{};
		tool_result_init(&result);
		ASSERT_EQ(tool_exec(&registry, "exec",
			host ? "{\"command\":\"cat\",\"host\":true,\"service\":true,\"yield_time_ms\":1}" :
			"{\"command\":\"cat\",\"service\":true,\"yield_time_ms\":1}", &result), 0);
		const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(result.data, "session_id"));
		ASSERT_NE(id, nullptr);
		std::string session = id;
		tool_context_clear_turn_grants(context);
		user.verdict = TOOL_OP_DENY;
		std::string input = "{\"action\":\"write\",\"session_id\":\"" + session +
			"\",\"input\":\"blocked\"}";
		EXPECT_EQ(tool_exec(&registry, "process", input.c_str(), &result), -EACCES);
		EXPECT_EQ(user.calls, host ? 2 : 1);
		std::string stop = "{\"action\":\"kill\",\"session_id\":\"" + session + "\"}";
		EXPECT_EQ(tool_exec(&registry, "process", stop.c_str(), &result), 0);
		tool_result_cleanup(&result);
		tool_registry_cleanup(&registry);
		tool_context_destroy(context);
	}
}

TEST_F(RuntimeFacadeTest, LegacyPermissionFlagsMigrateWithoutKeepingAnotherAuthority)
{
	WriteConfig("[exec]\nnetwork=true\nallow_host_execution=true\n"
		"[react]\nhitl_enabled=false\nhitl_auto_approve_media=true\n");
	struct config legacy{};
	ASSERT_EQ(config_load(nullptr, &legacy, config_path.c_str()), 0);
	EXPECT_EQ(legacy.permissions.access, PERMISSION_WORKSPACE);
	EXPECT_EQ(legacy.permissions.approval, PERMISSION_ON_REQUEST);
	EXPECT_EQ(legacy.permissions.reviewer, PERMISSION_USER);
	WriteConfig("[permissions]\naccess='read_only'\napproval='never'\nreviewer='model'\n"
		"[exec]\nnetwork=true\nallow_host_execution=true\n[react]\nhitl_enabled=false\n");
	ASSERT_EQ(config_load(nullptr, &legacy, config_path.c_str()), 0);
	EXPECT_EQ(legacy.permissions.access, PERMISSION_READ_ONLY);
	EXPECT_EQ(legacy.permissions.approval, PERMISSION_NEVER);
	EXPECT_EQ(legacy.permissions.reviewer, PERMISSION_MODEL);
}

TEST(Permission, NativeExtensionCannotClaimRestrictedProcessEnforcement)
{
	struct ext extension{};
	extension.enabled = 1;
	std::snprintf(extension.manifest.type, sizeof(extension.manifest.type), "so");
	extension.run = [](const char *, char **) { return 0; };
	struct sandbox_config limits{};
	limits.path_policy_enabled = 1;
	char *output = nullptr;
	EXPECT_EQ(ext_run_bounded(&extension, "{}", &output, nullptr, nullptr, &limits), -ENOTSUP);
	EXPECT_EQ(output, nullptr);
}

TEST_F(RuntimeFacadeTest, ExecutableExtensionIntersectionEnforcesReadOnlyAtTheOsBoundary)
{
	std::string script = directory + "/bounded.sh";
	std::string target = directory + "/forbidden";
	std::ofstream file(script);
	file << "#!/bin/sh\nIFS= read -r request\nprintf blocked > '" << target <<
		"' 2>/dev/null\nprintf '{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"bounded\":true}}'\n";
	file.close();
	ASSERT_EQ(chmod(script.c_str(), 0700), 0);
	struct ext extension{};
	extension.enabled = 1;
	std::snprintf(extension.path, sizeof(extension.path), "%s", directory.c_str());
	std::snprintf(extension.exec_path, sizeof(extension.exec_path), "%s", script.c_str());
	std::snprintf(extension.manifest.type, sizeof(extension.manifest.type), "exec");
	extension.manifest.permissions = EXT_PERM_EXEC | EXT_PERM_FILESYS;
	char *manifest_paths[] = {const_cast<char *>(directory.c_str())};
	extension.manifest.allowed_paths = manifest_paths;
	extension.manifest.allowed_paths_count = 1;
	auto *context = tool_context_create(nullptr, directory.c_str(), directory.c_str());
	ASSERT_NE(context, nullptr);
	struct permission_policy policy{PERMISSION_READ_ONLY, PERMISSION_NEVER, PERMISSION_USER};
	ASSERT_EQ(tool_context_set_permission_policy(context, &policy), 0);
	struct arena *arena = arena_create(0);
	ASSERT_NE(arena, nullptr);
	struct sandbox_config limits{};
	ASSERT_EQ(sandbox_permission_prepare(context, "bounded", nullptr, arena, &limits), 0);
	char *output = nullptr;
	EXPECT_EQ(ext_run_bounded(&extension, "{}", &output, nullptr, nullptr, &limits), 0);
	ASSERT_NE(output, nullptr);
	EXPECT_NE(std::string(output).find("bounded"), std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(target));
	free(output);
	arena_destroy(arena);
	tool_context_destroy(context);
}
