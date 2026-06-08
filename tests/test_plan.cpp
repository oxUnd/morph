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
