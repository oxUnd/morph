#include <gtest/gtest.h>

extern "C" {
#include "event/event.h"
#include "util/json.h"
}

TEST(EventTest, TypeName)
{
	EXPECT_STREQ("startup", morph_event_type_name(MORPH_EVENT_STARTUP));
	EXPECT_STREQ("react", morph_event_type_name(MORPH_EVENT_REACT));
	EXPECT_STREQ("tool", morph_event_type_name(MORPH_EVENT_TOOL));
	EXPECT_STREQ("mcp", morph_event_type_name(MORPH_EVENT_MCP));
	EXPECT_STREQ("hitl", morph_event_type_name(MORPH_EVENT_HITL));
	EXPECT_STREQ("task", morph_event_type_name(MORPH_EVENT_TASK));
	EXPECT_STREQ("command", morph_event_type_name(MORPH_EVENT_COMMAND));
	EXPECT_STREQ("interaction",
		     morph_event_type_name(MORPH_EVENT_INTERACTION));
	EXPECT_STREQ("unknown", morph_event_type_name((enum morph_event_type)99));
}

TEST(EventTest, SerializesStructuredPayload)
{
	struct json *data = json_object();
	ASSERT_NE(nullptr, data);
	json_add_string(data, "server", "filesystem");
	json_add_number(data, "tools", 4);

	struct morph_event ev = {
		MORPH_EVENT_MCP,
		"mcp.ready",
		"ready",
		"MCP server ready",
		data,
		"turn_test",
	};

	char *json = morph_event_to_json_string(&ev);
	ASSERT_NE(nullptr, json);

	struct json *root = json_parse_prefix(json, NULL, NULL);
	ASSERT_NE(nullptr, root);
	EXPECT_STREQ("mcp", json_string_value(json_member_ci(root, "type")));
	EXPECT_STREQ("mcp.ready", json_string_value(json_member_ci(root, "name")));
	EXPECT_STREQ("ready", json_string_value(json_member_ci(root, "phase")));
	EXPECT_STREQ("MCP server ready", json_string_value(json_member_ci(root, "message")));
	EXPECT_STREQ("turn_test", json_string_value(json_member_ci(root, "turn_id")));
	struct json *payload = json_member_ci(root, "data");
	ASSERT_TRUE(json_is_object_value(payload));
	EXPECT_STREQ("filesystem", json_string_value(json_member_ci(payload, "server")));
	EXPECT_EQ(4, json_int_value(json_member_ci(payload, "tools")));

	json_free(root);
	free(json);
	json_free(data);
}

TEST(EventTest, RecorderUsesEventSerialization)
{
	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));

	struct morph_event ev = {
		MORPH_EVENT_STARTUP,
		"startup.begin",
		"begin",
		nullptr,
		nullptr,
	};

	EXPECT_EQ(0, morph_event_recorder_cb(&ev, &rec));
	ASSERT_EQ(1u, morph_event_recorder_count(&rec));
	const char *json = morph_event_recorder_get(&rec, 0);
	ASSERT_NE(nullptr, json);

	struct json *root = json_parse_prefix(json, NULL, NULL);
	ASSERT_NE(nullptr, root);
	EXPECT_STREQ("startup", json_string_value(json_member_ci(root, "type")));
	EXPECT_STREQ("startup.begin", json_string_value(json_member_ci(root, "name")));
	EXPECT_TRUE(json_is_object_value(json_member_ci(root, "data")));

	json_free(root);
	morph_event_recorder_cleanup(&rec);
}
