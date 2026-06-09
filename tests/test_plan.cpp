#include <gtest/gtest.h>

#include "agent/plan.h"
#include "agent/tool.h"
#include "agent/tools/plan.h"

#include <stdlib.h>
#include <string.h>
#include <string>

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

	EXPECT_GT(strlen(result.text.data), 8192u);
	EXPECT_NE(std::string(result.text.data).find("32. step-32"), std::string::npos);

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
