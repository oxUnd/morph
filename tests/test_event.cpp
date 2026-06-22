#include <gtest/gtest.h>

extern "C" {
#include "event/event.h"
#include "cJSON.h"
}

TEST(EventTest, TypeName)
{
	EXPECT_STREQ("startup", morph_event_type_name(MORPH_EVENT_STARTUP));
	EXPECT_STREQ("react", morph_event_type_name(MORPH_EVENT_REACT));
	EXPECT_STREQ("tool", morph_event_type_name(MORPH_EVENT_TOOL));
	EXPECT_STREQ("mcp", morph_event_type_name(MORPH_EVENT_MCP));
	EXPECT_STREQ("task", morph_event_type_name(MORPH_EVENT_TASK));
	EXPECT_STREQ("unknown", morph_event_type_name((enum morph_event_type)99));
}

TEST(EventTest, SerializesStructuredPayload)
{
	cJSON *data = cJSON_CreateObject();
	ASSERT_NE(nullptr, data);
	cJSON_AddStringToObject(data, "server", "filesystem");
	cJSON_AddNumberToObject(data, "tools", 4);

	struct morph_event ev = {
		MORPH_EVENT_MCP,
		"mcp.ready",
		"ready",
		"MCP server ready",
		data,
	};

	char *json = morph_event_to_json_string(&ev);
	ASSERT_NE(nullptr, json);

	cJSON *root = cJSON_Parse(json);
	ASSERT_NE(nullptr, root);
	EXPECT_STREQ("mcp", cJSON_GetObjectItem(root, "type")->valuestring);
	EXPECT_STREQ("mcp.ready", cJSON_GetObjectItem(root, "name")->valuestring);
	EXPECT_STREQ("ready", cJSON_GetObjectItem(root, "phase")->valuestring);
	EXPECT_STREQ("MCP server ready",
		     cJSON_GetObjectItem(root, "message")->valuestring);
	cJSON *payload = cJSON_GetObjectItem(root, "data");
	ASSERT_TRUE(cJSON_IsObject(payload));
	EXPECT_STREQ("filesystem",
		     cJSON_GetObjectItem(payload, "server")->valuestring);
	EXPECT_EQ(4, cJSON_GetObjectItem(payload, "tools")->valueint);

	cJSON_Delete(root);
	free(json);
	cJSON_Delete(data);
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

	cJSON *root = cJSON_Parse(json);
	ASSERT_NE(nullptr, root);
	EXPECT_STREQ("startup", cJSON_GetObjectItem(root, "type")->valuestring);
	EXPECT_STREQ("startup.begin",
		     cJSON_GetObjectItem(root, "name")->valuestring);
	EXPECT_TRUE(cJSON_IsObject(cJSON_GetObjectItem(root, "data")));

	cJSON_Delete(root);
	morph_event_recorder_cleanup(&rec);
}
