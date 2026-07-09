#include <gtest/gtest.h>

#include "agent/plan.h"
#include "agent/tool.h"
#include "agent/tools/plan.h"

#include <stdlib.h>
#include <string.h>
#include <string>

static std::string selected_plan_id(cJSON *data)
{
	cJSON *id = cJSON_GetObjectItem(data, "selected_plan_id");
	if (!cJSON_IsString(id) || !id->valuestring)
		return "";
	return id->valuestring;
}

static cJSON *find_plan_by_id(cJSON *data, const std::string &id)
{
	cJSON *plans = cJSON_GetObjectItem(data, "plans");
	if (!cJSON_IsArray(plans))
		return nullptr;
	int count = cJSON_GetArraySize(plans);
	for (int i = 0; i < count; i++) {
		cJSON *plan = cJSON_GetArrayItem(plans, i);
		cJSON *plan_id = cJSON_GetObjectItem(plan, "id");
		if (cJSON_IsString(plan_id) && id == plan_id->valuestring)
			return plan;
	}
	return nullptr;
}

static const char *first_step_status(cJSON *plan)
{
	cJSON *steps = cJSON_GetObjectItem(plan, "steps");
	if (!cJSON_IsArray(steps))
		return "";
	cJSON *step = cJSON_GetArrayItem(steps, 0);
	cJSON *status = cJSON_GetObjectItem(step, "status");
	if (!cJSON_IsString(status) || !status->valuestring)
		return "";
	return status->valuestring;
}

TEST(PlanTool, LongCreateOutputIsNotTruncated)
{
	struct tool_registry tools;
	struct plan_registry plans;

	tool_registry_init(&tools);
	plan_registry_init(&plans);
	ASSERT_EQ(plan_tool_init(&tools, &plans, nullptr), 0);

	std::string args =
		"{\"command\":\"create\",\"name\":\"long\",\"goal\":\"g\","
		"\"steps\":[";
	for (int i = 0; i < PLAN_MAX_STEPS; i++) {
		if (i > 0)
			args += ",";
		std::string desc = "step-" + std::to_string(i + 1) + " ";
		desc.append(240, (char)('a' + (i % 26)));
		args += "\"";
		args += desc;
		args += "\"";
	}
	args += "]}";

	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "plan", args.c_str(), &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	ASSERT_NE(result.data, nullptr);
	ASSERT_NE(result.ui, nullptr);

	EXPECT_GT(strlen(result.text.data), 8192u);
	cJSON *plans_json = cJSON_GetObjectItem(result.data, "plans");
	ASSERT_TRUE(cJSON_IsArray(plans_json));
	cJSON *first_plan = cJSON_GetArrayItem(plans_json, 0);
	ASSERT_NE(first_plan, nullptr);
	cJSON *plan_id = cJSON_GetObjectItem(first_plan, "id");
	ASSERT_TRUE(cJSON_IsString(plan_id));
	EXPECT_NE(std::string(plan_id->valuestring).find("pln_"),
		  std::string::npos);
	cJSON *steps_json = cJSON_GetObjectItem(first_plan, "steps");
	ASSERT_TRUE(cJSON_IsArray(steps_json));
	EXPECT_EQ(cJSON_GetArraySize(steps_json), PLAN_MAX_STEPS);
	cJSON *last_step = cJSON_GetArrayItem(steps_json, PLAN_MAX_STEPS - 1);
	ASSERT_NE(last_step, nullptr);
	cJSON *last_desc = cJSON_GetObjectItem(last_step, "description");
	ASSERT_TRUE(cJSON_IsString(last_desc));
	EXPECT_NE(std::string(last_desc->valuestring).find("step-32"),
		  std::string::npos);
	cJSON *component = cJSON_GetObjectItem(result.ui, "component");
	ASSERT_TRUE(cJSON_IsString(component));
	EXPECT_STREQ(component->valuestring, "plan");

	tool_result_cleanup(&result);
	tool_registry_cleanup(&tools);
}

TEST(PlanTool, RegistryScopedPlanRegistry)
{
	struct tool_registry tools1;
	struct tool_registry tools2;
	struct plan_registry plans1;
	struct plan_registry plans2;

	tool_registry_init(&tools1);
	tool_registry_init(&tools2);
	plan_registry_init(&plans1);
	plan_registry_init(&plans2);
	ASSERT_EQ(plan_tool_init(&tools1, &plans1, nullptr), 0);
	ASSERT_EQ(plan_tool_init(&tools2, &plans2, nullptr), 0);

	struct tool_result result1;
	struct tool_result result2;
	tool_result_init(&result1);
	tool_result_init(&result2);

	ASSERT_EQ(tool_exec(&tools1, "plan",
		"{\"command\":\"create\",\"name\":\"first\","
		"\"steps\":[\"one\"]}", &result1), 0);
	ASSERT_EQ(tool_exec(&tools2, "plan",
		"{\"command\":\"create\",\"name\":\"second\","
		"\"steps\":[\"two\"]}", &result2), 0);

	tool_result_clear(&result1);
	tool_result_clear(&result2);

	ASSERT_EQ(tool_exec(&tools1, "plan",
		"{\"command\":\"list\"}", &result1), 0);
	ASSERT_EQ(tool_exec(&tools2, "plan",
		"{\"command\":\"list\"}", &result2), 0);

	ASSERT_NE(result1.text.data, nullptr);
	ASSERT_NE(result2.text.data, nullptr);
	ASSERT_NE(result1.data, nullptr);
	ASSERT_NE(result2.data, nullptr);
	EXPECT_NE(std::string(result1.text.data).find("first"),
		  std::string::npos);
	EXPECT_EQ(std::string(result1.text.data).find("second"),
		  std::string::npos);
	EXPECT_NE(std::string(result2.text.data).find("second"),
		  std::string::npos);
	EXPECT_EQ(std::string(result2.text.data).find("first"),
		  std::string::npos);

	tool_result_cleanup(&result1);
	tool_result_cleanup(&result2);
	tool_registry_cleanup(&tools1);
	tool_registry_cleanup(&tools2);
}

TEST(PlanTool, PlanIdSelectsDuplicateNames)
{
	struct tool_registry tools;
	struct plan_registry plans;

	tool_registry_init(&tools);
	plan_registry_init(&plans);
	ASSERT_EQ(plan_tool_init(&tools, &plans, nullptr), 0);

	struct tool_result result;
	tool_result_init(&result);

	ASSERT_EQ(tool_exec(&tools, "plan",
		"{\"command\":\"create\",\"name\":\"same\","
		"\"steps\":[\"one\"]}", &result), 0);
	std::string first_id = selected_plan_id(result.data);
	ASSERT_FALSE(first_id.empty());
	tool_result_clear(&result);

	ASSERT_EQ(tool_exec(&tools, "plan",
		"{\"command\":\"create\",\"name\":\"same\","
		"\"steps\":[\"two\"]}", &result), 0);
	std::string second_id = selected_plan_id(result.data);
	ASSERT_FALSE(second_id.empty());
	ASSERT_NE(first_id, second_id);
	tool_result_clear(&result);

	std::string update = "{\"command\":\"update\",\"plan_id\":\"" +
		second_id + "\",\"step_id\":1,\"status\":\"completed\"}";
	ASSERT_EQ(tool_exec(&tools, "plan", update.c_str(), &result), 0);
	cJSON *first = find_plan_by_id(result.data, first_id);
	cJSON *second = find_plan_by_id(result.data, second_id);
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);
	EXPECT_STREQ(first_step_status(first), "in_progress");
	EXPECT_STREQ(first_step_status(second), "completed");
	tool_result_clear(&result);

	std::string get = "{\"command\":\"get\",\"plan_id\":\"" +
		second_id + "\"}";
	ASSERT_EQ(tool_exec(&tools, "plan", get.c_str(), &result), 0);
	EXPECT_EQ(selected_plan_id(result.data), second_id);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(std::string(result.text.data).find(second_id),
		  std::string::npos);

	tool_result_cleanup(&result);
	tool_registry_cleanup(&tools);
}

TEST(PlanTool, LegacyNameUpdateStillWorks)
{
	struct tool_registry tools;
	struct plan_registry plans;

	tool_registry_init(&tools);
	plan_registry_init(&plans);
	ASSERT_EQ(plan_tool_init(&tools, &plans, nullptr), 0);

	struct tool_result result;
	tool_result_init(&result);

	ASSERT_EQ(tool_exec(&tools, "plan",
		"{\"command\":\"create\",\"name\":\"legacy\","
		"\"steps\":[\"one\"]}", &result), 0);
	tool_result_clear(&result);

	ASSERT_EQ(tool_exec(&tools, "plan",
		"{\"command\":\"update\",\"plan\":\"legacy\","
		"\"step_id\":1,\"status\":\"completed\"}", &result), 0);
	ASSERT_NE(result.data, nullptr);
	cJSON *plans_json = cJSON_GetObjectItem(result.data, "plans");
	ASSERT_TRUE(cJSON_IsArray(plans_json));
	cJSON *plan = cJSON_GetArrayItem(plans_json, 0);
	ASSERT_NE(plan, nullptr);
	EXPECT_STREQ(first_step_status(plan), "completed");

	tool_result_cleanup(&result);
	tool_registry_cleanup(&tools);
}
