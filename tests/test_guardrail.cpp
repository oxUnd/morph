#include <gtest/gtest.h>

#include "agent/guardrail.h"
#include "agent/guardrail_evidence.h"
#include "agent/react.h"
#include "config/config.h"
#include "models/llm.h"
#include "errors.h"
#include <nb/file.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

TEST(GuardrailEvidence, WhitespaceAndCodeExamplesHaveDistinctSemantics)
{
	struct guardrail_config config{};
	config.enabled = 1;
	guardrail_register_builtin_rules(&config);
	struct guardrail_eval_ctx eval{};
	for (const char *answer : {" \n\t", "\xe3\x80\x80\xc2\xa0"}) {
		eval.proposed_answer = answer;
		EXPECT_EQ(guardrail_run_hook(&config, GUARDRAIL_HOOK_OUTPUT, &eval).verdict,
			  GUARDRAIL_FAIL);
	}
	for (const char *answer : {"Example: `[file](/nonexistent/example.txt)`",
				   "```markdown\n[file](/nonexistent/example.txt)\n```"}) {
		eval.proposed_answer = answer;
		EXPECT_EQ(guardrail_run_hook(&config, GUARDRAIL_HOOK_OUTPUT, &eval).verdict,
			  GUARDRAIL_PASS);
	}
}

TEST(GuardrailEvidence, MarkdownTargetsResolveWorkdirEscapingAndSourceLocations)
{
	char directory[] = "/tmp/morph-guardrail-links-XXXXXX";
	ASSERT_NE(mkdtemp(directory), nullptr);
	std::string path = std::string(directory) + "/source (copy).c";
	ASSERT_EQ(nb_file_write_all(path.c_str(), "x", 1, nullptr), NB_OK);
	struct guardrail_config config{};
	config.enabled = 1;
	guardrail_register_builtin_rules(&config);
	struct guardrail_eval_ctx eval{};
	eval.workdir = directory;
	for (const char *answer :
	     {"[source](<source (copy).c:12:3>)", "[source](source%20%28copy%29.c#section)",
	      "[source][ref]\n\n[ref]: <source (copy).c>"}) {
		eval.proposed_answer = answer;
		EXPECT_EQ(guardrail_run_hook(&config, GUARDRAIL_HOOK_OUTPUT, &eval).verdict,
			  GUARDRAIL_PASS)
			<< answer;
	}
	unlink(path.c_str());
	rmdir(directory);
}

TEST(GuardrailEvidence, FullCountsAndLateSuccessSurviveBoundedTrajectory)
{
	std::vector<struct react_step> steps(130);
	for (size_t i = 0; i < steps.size(); i++) {
		steps[i].type = i % 2 == 0 ? REACT_STEP_ACTION : REACT_STEP_OBSERVATION;
		steps[i].tool_name = i % 2 == 0 ? const_cast<char *>("test_tool") : nullptr;
		steps[i].error_code = i % 2 == 1 && i < 129 ? NB_ERR_IO : 0;
		steps[i].next = i + 1 < steps.size() ? &steps[i + 1] : nullptr;
	}
	struct guardrail_config config{};
	config.enabled = 1;
	guardrail_register_builtin_rules(&config);
	struct guardrail_eval_ctx eval{};
	eval.user_input = "Inspect the final result.";
	eval.proposed_answer = "The last call succeeded.";
	eval.steps = steps.data();
	EXPECT_EQ(guardrail_run_hook(&config, GUARDRAIL_HOOK_OUTPUT, &eval).verdict,
		  GUARDRAIL_PASS);
	cJSON *evidence NB_CLEANUP(cJSON_DeleteOwner) = guardrail_evidence_create(&eval);
	ASSERT_NE(evidence, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(evidence, "total_calls")->valueint, 65);
	EXPECT_EQ(cJSON_GetObjectItem(evidence, "succeeded_calls")->valueint, 1);
	EXPECT_EQ(cJSON_GetObjectItem(evidence, "failed_calls")->valueint, 64);
	EXPECT_EQ(cJSON_GetObjectItem(evidence, "omitted_calls")->valueint, 33);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(evidence, "evidence_incomplete")));
}

TEST(GuardrailEvidence, OutOfOrderResultsMatchIdsAndPendingCallsStayPending)
{
	struct react_step steps[4]{};
	steps[0].type = steps[1].type = REACT_STEP_ACTION;
	steps[0].tool_name = const_cast<char *>("first");
	steps[1].tool_name = const_cast<char *>("second");
	steps[0].tool_call_id = const_cast<char *>("a");
	steps[1].tool_call_id = const_cast<char *>("b");
	steps[2].type = steps[3].type = REACT_STEP_OBSERVATION;
	steps[2].tool_call_id = const_cast<char *>("b");
	steps[3].tool_call_id = const_cast<char *>("a");
	steps[2].error_code = NB_ERR_IO;
	for (size_t i = 0; i < 3; i++)
		steps[i].next = &steps[i + 1];
	struct guardrail_eval_ctx eval{};
	eval.steps = steps;
	cJSON *evidence NB_CLEANUP(cJSON_DeleteOwner) = guardrail_evidence_create(&eval);
	ASSERT_NE(evidence, nullptr);
	cJSON *calls = cJSON_GetObjectItem(evidence, "recent_calls");
	EXPECT_STREQ(
		cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(calls, 0), "outcome")),
		"succeeded");
	EXPECT_STREQ(
		cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(calls, 1), "outcome")),
		"failed");
	steps[2].next = nullptr;
	cJSON *pending NB_CLEANUP(cJSON_DeleteOwner) = guardrail_evidence_create(&eval);
	ASSERT_NE(pending, nullptr);
	calls = cJSON_GetObjectItem(pending, "recent_calls");
	EXPECT_STREQ(
		cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(calls, 0), "outcome")),
		"pending");
}

TEST(GuardrailEvidence, ArtifactChecksCoverOpaqueToolsAndPreserveEmptyTextFiles)
{
	char path[] = "/tmp/morph-guardrail-artifact-XXXXXX";
	int fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	close(fd);
	struct tool_artifact_list artifacts{};
	artifacts.count = 1;
	std::strcpy(artifacts.items[0].path, path);
	struct guardrail_config config{};
	config.enabled = 1;
	guardrail_register_builtin_rules(&config);
	struct guardrail_eval_ctx eval{};
	eval.tool_name = "remote_extension";
	eval.tool_artifacts = &artifacts;
	EXPECT_EQ(guardrail_run_hook(&config, GUARDRAIL_HOOK_TOOL_OUTPUT, &eval).verdict,
		  GUARDRAIL_PASS);
	artifacts.items[0].kind = TOOL_ARTIFACT_IMAGE;
	auto result = guardrail_run_hook(&config, GUARDRAIL_HOOK_TOOL_OUTPUT, &eval);
	EXPECT_EQ(result.verdict, GUARDRAIL_FAIL);
	ASSERT_NE(result.triggered_rule, nullptr);
	EXPECT_STREQ(result.triggered_rule->name, "artifact_invalid");
	ASSERT_EQ(nb_file_write_all(path, "not an image", 12, nullptr), NB_OK);
	EXPECT_EQ(guardrail_run_hook(&config, GUARDRAIL_HOOK_TOOL_OUTPUT, &eval).verdict,
		  GUARDRAIL_FAIL);
	std::strcpy(artifacts.items[0].mime, "image/webp");
	cJSON *evidence NB_CLEANUP(cJSON_DeleteOwner) = guardrail_evidence_create(&eval);
	ASSERT_NE(evidence, nullptr);
	cJSON *current = cJSON_GetObjectItem(evidence, "current_call");
	cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItem(current, "artifacts"), 0);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(item, "verification")),
		     "format_unverified");
	std::string webp = std::string(path) + ".WEBP";
	ASSERT_EQ(rename(path, webp.c_str()), 0);
	artifacts.items[0].mime[0] = '\0';
	std::strcpy(artifacts.items[0].path, webp.c_str());
	nb_file_info info{};
	EXPECT_EQ(guardrail_artifact_inspect(&artifacts.items[0], nullptr, &info),
		  GUARDRAIL_ARTIFACT_FORMAT_UNVERIFIED);
	unlink(webp.c_str());
	unlink(path);
}

TEST(GuardrailEvidence, CheckerErrorsAreObservableAndCanBlock)
{
	struct model checker{};
	checker.chat = [](struct model *self, const char *, const char **, int,
			  const struct model_chat_options *, sse_callback callback,
			  void *data) -> int {
		const char *response = static_cast<const char *>(self->handle);
		return !strcmp(response, "timeout") ? NB_ERR_TIMEOUT : callback(response, data);
	};
	struct guardrail_config config{};
	config.enabled = 1;
	config.llm = &checker;
	ASSERT_EQ(guardrail_rule_register(&config, "strict", GUARDRAIL_HOOK_OUTPUT,
					  GUARDRAIL_RULE_LLM, nullptr, "Validate the result",
					  nullptr, nullptr),
		  NB_OK);
	struct guardrail_eval_ctx eval{};
	eval.proposed_answer = "result";
	for (const char *response :
	     {"timeout", "not JSON", "{}", "{\"pass\":1}", "{\"pass\":true,\"pass\":false}",
	      "{\"pass\":true} trailing"}) {
		checker.handle = const_cast<char *>(response);
		config.rules[0].block_on_error = 0;
		auto result = guardrail_run_hook(&config, GUARDRAIL_HOOK_OUTPUT, &eval);
		EXPECT_EQ(result.verdict, GUARDRAIL_PASS) << response;
		EXPECT_LT(result.check_error, 0) << response;
		config.rules[0].block_on_error = 1;
		result = guardrail_run_hook(&config, GUARDRAIL_HOOK_OUTPUT, &eval);
		EXPECT_EQ(result.verdict, GUARDRAIL_FAIL) << response;
		EXPECT_TRUE(result.stop) << response;
		EXPECT_LT(result.check_error, 0) << response;
	}
}

TEST(GuardrailEvidence, CheckerBudgetsAndCancellationDoNotMutateTheClient)
{
	int calls = 0;
	struct model checker{};
	checker.handle = &calls;
	checker.retry_count = 7;
	checker.chat = [](struct model *self, const char *policy, const char **messages, int,
			  const struct model_chat_options *options, sse_callback callback,
			  void *data) -> int {
		++*static_cast<int *>(self->handle);
		EXPECT_EQ(self->retry_count, 0);
		EXPECT_EQ(options->timeout_seconds, 9);
		EXPECT_EQ(options->max_tokens, 128);
		EXPECT_STREQ(options->usage_phase, "guardrail");
		EXPECT_EQ(strstr(policy, "UNTRUSTED_PAYLOAD"), nullptr);
		EXPECT_NE(strstr(messages[0], "UNTRUSTED_PAYLOAD"), nullptr);
		return callback("{\"pass\":true}", data);
	};
	struct guardrail_config config{};
	config.enabled = 1;
	config.llm = &checker;
	config.timeout_seconds = 9;
	config.max_check_tokens = 128;
	config.max_llm_checks = 1;
	ASSERT_EQ(guardrail_rule_register(&config, "bounded", GUARDRAIL_HOOK_OUTPUT,
					  GUARDRAIL_RULE_LLM, nullptr, "Validate result", nullptr,
					  nullptr),
		  NB_OK);
	config.rules[0].block_on_error = 1;
	struct nb_cancel token{};
	int count = 0;
	struct guardrail_eval_ctx eval{};
	eval.proposed_answer = "UNTRUSTED_PAYLOAD";
	eval.cancel_token = &token;
	eval.llm_check_count = &count;
	EXPECT_EQ(guardrail_run_hook(&config, GUARDRAIL_HOOK_OUTPUT, &eval).verdict,
		  GUARDRAIL_PASS);
	auto result = guardrail_run_hook(&config, GUARDRAIL_HOOK_OUTPUT, &eval);
	EXPECT_EQ(result.check_error, MORPH_ERR_REQUEST_BUDGET);
	EXPECT_TRUE(result.stop);
	count = 0;
	nb_cancel_request(&token);
	result = guardrail_run_hook(&config, GUARDRAIL_HOOK_OUTPUT, &eval);
	EXPECT_EQ(result.check_error, NB_ERR_CANCELLED);
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(checker.retry_count, 7);
	EXPECT_EQ(checker.http.cancel_token, nullptr);
}

TEST(GuardrailEvidence, ExecCheckersReceiveContextWithoutBreakingLegacyText)
{
	char path[] = "/tmp/morph-context-checker-XXXXXX";
	int fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	close(fd);
	const char *script = "#!/usr/bin/env python3\n"
			     "import json,sys\n"
			     "request=json.loads(sys.stdin.readline())\n"
			     "p=request['params']\n"
			     "assert p['text']=='candidate'\n"
			     "assert p['context']['user_objective']=='original objective'\n"
			     "assert p['context']['candidate_answer']=='candidate'\n"
			     "print(json.dumps({'jsonrpc':'2.0','id':request['id'],"
			     "'result':{'pass':True}}))\n";
	ASSERT_EQ(nb_file_write_all(path, script, strlen(script), nullptr), NB_OK);
	ASSERT_EQ(chmod(path, 0700), 0);
	struct guardrail_config config{};
	config.enabled = 1;
	ASSERT_EQ(guardrail_rule_register(&config, "context", GUARDRAIL_HOOK_OUTPUT,
					  GUARDRAIL_RULE_EXT, nullptr, "Check context", path,
					  nullptr),
		  NB_OK);
	config.rules[0].block_on_error = 1;
	struct guardrail_eval_ctx eval{};
	eval.user_input = "original objective";
	eval.proposed_answer = "candidate";
	auto result = guardrail_run_hook(&config, GUARDRAIL_HOOK_OUTPUT, &eval);
	EXPECT_EQ(result.verdict, GUARDRAIL_PASS);
	EXPECT_EQ(result.check_error, 0);
	unlink(path);
}

TEST(GuardrailEvidence, ConfigLoadsFailurePoliciesAndBoundsCheckerSettings)
{
	char path[] = "/tmp/morph-context-config-XXXXXX";
	int fd = mkstemp(path);
	ASSERT_GE(fd, 0);
	close(fd);
	const char *text =
		"[react]\nguardrail_timeout_seconds=9\nguardrail_max_check_tokens=128\n"
		"guardrail_max_llm_checks=4\n[[react.guardrail_llm_rules]]\n"
		"name='action_monitor'\nhook='tool_input'\non_error='block'\non_failure='block'\n"
		"description='Check the action against the user objective'\n";
	ASSERT_EQ(nb_file_write_all(path, text, strlen(text), nullptr), NB_OK);
	auto config = std::make_unique<struct config>();
	ASSERT_EQ(config_load(nullptr, config.get(), path), NB_OK);
	EXPECT_EQ(config->react.guardrail_timeout_seconds, 9);
	EXPECT_EQ(config->react.guardrail_max_check_tokens, 128);
	EXPECT_EQ(config->react.guardrail_max_llm_checks, 4);
	EXPECT_STREQ(config->react.guardrail_llm_rules[0].hook, "tool_input");
	EXPECT_STREQ(config->react.guardrail_llm_rules[0].on_error, "block");
	EXPECT_STREQ(config->react.guardrail_llm_rules[0].on_failure, "block");
	for (const char *invalid :
	     {"[react]\nguardrail_timeout_seconds=0\n", "[react]\nguardrail_max_llm_checks=-1\n",
	      "[[react.guardrail_llm_rules]]\nname='x'\non_error='invalid'\n",
	      "[[react.guardrail_ext_rules]]\nname='x'\non_failure='invalid'\n"}) {
		ASSERT_EQ(nb_file_write_all(path, invalid, strlen(invalid), nullptr), NB_OK);
		struct config_validation_error error{};
		EXPECT_NE(config_validate_file(path, &error), NB_OK);
	}
	unlink(path);
}
