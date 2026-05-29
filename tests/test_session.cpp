#include <gtest/gtest.h>
#include "db/database.h"
#include "session.h"
#include <cctype>
#include <cstdio>
#include <cstring>

class SessionTest : public ::testing::Test {
protected:
	struct db db;
	char db_path[256];
	void SetUp() override {
		snprintf(db_path, sizeof(db_path), "/tmp/ma_test_session_%d.db", getpid());
		std::remove(db_path);
		int rc = db_open(&db, db_path);
		ASSERT_EQ(rc, 0);
		rc = db_init_schema(&db);
		ASSERT_EQ(rc, 0);
	}
	void TearDown() override {
		db_close(&db);
		std::remove(db_path);
	}
};

TEST_F(SessionTest, CreateSession) {
	struct session s;
	int rc = session_create(&db, "test_session", "gpt-4o", &s);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(s.name, "test_session");
	EXPECT_STREQ(s.model, "gpt-4o");
	EXPECT_GT(s.id, 0);
	EXPECT_STRNE(s.display_id, "");
	EXPECT_EQ(strlen(s.display_id), 8);
	// hex format
	for (int i = 0; s.display_id[i]; i++)
		EXPECT_TRUE(isxdigit(s.display_id[i]));
}

TEST_F(SessionTest, CreateDuplicate) {
	struct session s1, s2;
	int rc = session_create(&db, "dup_session", "gpt-4o", &s1);
	EXPECT_EQ(rc, 0);
	rc = session_create(&db, "dup_session", "gpt-4o", &s2);
	EXPECT_NE(rc, 0);
}

TEST_F(SessionTest, GetByName) {
	struct session s;
	session_create(&db, "findme", "gpt-4o", &s);
	struct session found;
	int rc = session_get_by_name(&db, "findme", &found);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(found.name, "findme");
}

TEST_F(SessionTest, GetByNameNotFound) {
	struct session found;
	int rc = session_get_by_name(&db, "nonexistent", &found);
	EXPECT_NE(rc, 0);
}

TEST_F(SessionTest, GetById) {
	struct session s;
	session_create(&db, "byid", "gpt-4o", &s);
	struct session found;
	int rc = session_get_by_id(&db, s.id, &found);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(found.id, s.id);
}

TEST_F(SessionTest, GetByIdNotFound) {
	struct session found;
	int rc = session_get_by_id(&db, 99999, &found);
	EXPECT_NE(rc, 0);
}

TEST_F(SessionTest, ListSessions) {
	struct session s1, s2, s3;
	session_create(&db, "s1", "gpt-4o", &s1);
	session_create(&db, "s2", "claude-3.5", &s2);
	session_create(&db, "s3", "gpt-4o", &s3);
	struct session *list;
	int count = 0;
	int rc = session_list(&db, &list, &count, 0, NULL);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(count, 3);
	free(list);
}

TEST_F(SessionTest, RenameSession) {
	struct session s;
	session_create(&db, "original", "gpt-4o", &s);
	int rc = session_rename(&db, s.id, "renamed");
	EXPECT_EQ(rc, 0);
	struct session found;
	rc = session_get_by_name(&db, "renamed", &found);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(found.name, "renamed");
}

TEST_F(SessionTest, DeleteSession) {
	struct session s;
	session_create(&db, "to_delete", "gpt-4o", &s);
	int rc = session_delete(&db, s.id);
	EXPECT_EQ(rc, 0);
	struct session found;
	rc = session_get_by_name(&db, "to_delete", &found);
	EXPECT_NE(rc, 0);
}

TEST_F(SessionTest, UpdateModel) {
	struct session s;
	session_create(&db, "model_test", "gpt-4o", &s);
	int rc = session_update_model(&db, s.id, "claude-3.5");
	EXPECT_EQ(rc, 0);
	struct session found;
	session_get_by_id(&db, s.id, &found);
	EXPECT_STREQ(found.model, "claude-3.5");
}

TEST_F(SessionTest, NullParams) {
	struct session s;
	EXPECT_NE(session_create(nullptr, "test", "gpt-4o", &s), 0);
	EXPECT_NE(session_create(&db, nullptr, "gpt-4o", &s), 0);
}

TEST_F(SessionTest, AddMessage) {
	struct session s;
	session_create(&db, "msg_test", "gpt-4o", &s);
	int rc = message_add(&db, s.id, "user", "hello world", 3);
	EXPECT_EQ(rc, 0);
}

TEST_F(SessionTest, ListMessages) {
	struct session s;
	session_create(&db, "msg_list", "gpt-4o", &s);
	message_add(&db, s.id, "user", "hello", 1);
	message_add(&db, s.id, "assistant", "hi there", 2);
	int count = 0;
	struct message *msgs = message_list(&db, s.id, &count);
	EXPECT_EQ(count, 2);
	EXPECT_STREQ(msgs->role, "user");
	EXPECT_STREQ(msgs->content, "hello");
	message_free_list(msgs);
}

TEST_F(SessionTest, MessageCount) {
	struct session s;
	session_create(&db, "msg_count", "gpt-4o", &s);
	message_add(&db, s.id, "user", "hello", 1);
	message_add(&db, s.id, "assistant", "hi", 1);
	message_add(&db, s.id, "user", "how are you", 3);
	int count = message_count(&db, s.id);
	EXPECT_EQ(count, 3);
}

TEST_F(SessionTest, MessageFreeListNull) {
	EXPECT_NO_FATAL_FAILURE(message_free_list(nullptr));
}

TEST_F(SessionTest, AutoRenameNewSession) {
	struct session s;
	int rc = session_create(&db, "new", "gpt-4o", &s);
	ASSERT_EQ(rc, 0);
	int auto_named = 0;

	const char *input = "画一只猫";
	if (!auto_named && input[0] != '/') {
		char title[48];
		size_t len = strlen(input);
		size_t max_len = sizeof(title) - 4;
		if (len > max_len) {
			memcpy(title, input, max_len);
			title[max_len] = '\0';
			strcat(title, "...");
		} else {
			memcpy(title, input, len);
			title[len] = '\0';
		}
		session_rename(&db, s.id, title);
		strncpy(s.name, title, sizeof(s.name) - 1);
		auto_named = 1;
	}
	EXPECT_STREQ(s.name, "画一只猫");
	EXPECT_EQ(auto_named, 1);

	struct session loaded;
	rc = session_get_by_name(&db, "画一只猫", &loaded);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(loaded.name, "画一只猫");
}

TEST_F(SessionTest, AutoRenameExistingSession) {
	struct session s;
	int rc = session_create(&db, "new", "gpt-4o", &s);
	ASSERT_EQ(rc, 0);
	message_add(&db, s.id, "user", "old message", 2);

	int auto_named = 0;
	const char *input = "画一只猫";
	if (!auto_named && input[0] != '/') {
		char title[48];
		size_t len = strlen(input);
		size_t max_len = sizeof(title) - 4;
		if (len > max_len) {
			memcpy(title, input, max_len);
			title[max_len] = '\0';
			strcat(title, "...");
		} else {
			memcpy(title, input, len);
			title[len] = '\0';
		}
		session_rename(&db, s.id, title);
		strncpy(s.name, title, sizeof(s.name) - 1);
		auto_named = 1;
	}
	EXPECT_STREQ(s.name, "画一只猫");
	EXPECT_EQ(auto_named, 1);
}

TEST_F(SessionTest, DisplayIdGetByDisplayId) {
	struct session s;
	session_create(&db, "display_find", "gpt-4o", &s);
	ASSERT_STRNE(s.display_id, "");

	struct session found;
	int rc = session_get_by_display_id(&db, s.display_id, &found);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(found.id, s.id);
	EXPECT_STREQ(found.display_id, s.display_id);
	EXPECT_STREQ(found.name, s.name);

	rc = session_get_by_display_id(&db, "00000000", &found);
	EXPECT_NE(rc, 0);
}

TEST_F(SessionTest, DisplayIdUnique) {
	struct session s1, s2;
	session_create(&db, "unique1", "gpt-4o", &s1);
	session_create(&db, "unique2", "gpt-4o", &s2);
	// extremely unlikely to collide, but still…
	EXPECT_STRNE(s1.display_id, s2.display_id);
	EXPECT_EQ(strlen(s1.display_id), strlen(s2.display_id));
}

TEST_F(SessionTest, DisplayIdPersist) {
	struct session s;
	session_create(&db, "persist_test", "gpt-4o", &s);
	ASSERT_STRNE(s.display_id, "");

	struct session loaded;
	int rc = session_get_by_id(&db, s.id, &loaded);
	ASSERT_EQ(rc, 0);
	EXPECT_STREQ(loaded.display_id, s.display_id);

	rc = session_get_by_name(&db, "persist_test", &loaded);
	ASSERT_EQ(rc, 0);
	EXPECT_STREQ(loaded.display_id, s.display_id);
}

TEST_F(SessionTest, DisplayIdInList) {
	session_create(&db, "list_display", "gpt-4o", nullptr);
	struct session *list;
	int count;
	session_list(&db, &list, &count, 0, NULL);
	ASSERT_GE(count, 1);
	bool found = false;
	for (int i = 0; i < count; i++) {
		if (strcmp(list[i].name, "list_display") == 0) {
			found = true;
			EXPECT_EQ(strlen(list[i].display_id), 8);
			break;
		}
	}
	EXPECT_TRUE(found);
	free(list);
}

TEST_F(SessionTest, AutoNamedUnique) {
	/* Simulate cmd_new: generate unique name with timestamp */
	struct session s1, s2;
	char name1[256], name2[256];
	snprintf(name1, sizeof(name1), "new_%lld", (long long)time(NULL));
	int rc = session_create(&db, name1, "gpt-4o", &s1);
	EXPECT_EQ(rc, 0);

	/* Second call at a different time should not collide */
	sleep(1);
	snprintf(name2, sizeof(name2), "new_%lld", (long long)time(NULL));
	rc = session_create(&db, name2, "gpt-4o", &s2);
	EXPECT_EQ(rc, 0);
	EXPECT_STRNE(s1.name, s2.name);
}

TEST_F(SessionTest, AutoRenameFromUniqueName) {
	struct session s;
	char name[256];
	snprintf(name, sizeof(name), "new_%lld", (long long)time(NULL));
	int rc = session_create(&db, name, "gpt-4o", &s);
	ASSERT_EQ(rc, 0);

	/* Auto-rename on first user input (same logic as cli.c L2210-2227) */
	const char *input = "hello world";
	char title[48];
	size_t len = strlen(input);
	memcpy(title, input, len);
	title[len] = '\0';
	session_rename(&db, s.id, title);
	strncpy(s.name, title, sizeof(s.name) - 1);

	EXPECT_STREQ(s.name, "hello world");

	struct session loaded;
	rc = session_get_by_name(&db, "hello world", &loaded);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ(loaded.name, "hello world");
}

TEST_F(SessionTest, AutoRenameTruncation) {
	struct session s;
	int rc = session_create(&db, "new", "gpt-4o", &s);
	ASSERT_EQ(rc, 0);
	int auto_named = 0;

	const char *input = "这是一段非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常非常长的输入内容用来测试截断";
	if (!auto_named && input[0] != '/') {
		char title[48];
		size_t len = strlen(input);
		size_t max_len = sizeof(title) - 4;
		if (len > max_len) {
			memcpy(title, input, max_len);
			title[max_len] = '\0';
			strcat(title, "...");
		} else {
			memcpy(title, input, len);
			title[len] = '\0';
		}
		session_rename(&db, s.id, title);
		strncpy(s.name, title, sizeof(s.name) - 1);
		auto_named = 1;
	}
	EXPECT_EQ(auto_named, 1);
	EXPECT_EQ(strlen(s.name), 47);
	char *dots = strstr(s.name, "...");
	ASSERT_NE(dots, nullptr);
	EXPECT_STREQ(dots, "...");
}