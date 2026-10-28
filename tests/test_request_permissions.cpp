#include <gtest/gtest.h>

extern "C" {
#include "agent/tool_context.h"
#include "agent/tool.h"
#include "agent/tools/request_permissions.h"
#include "db/database.h"
#include "util/file.h"
}

#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

class DirectoryPermissionTest : public ::testing::Test {
protected:
	struct Approval {
		unsigned int access;
		std::string path;
		std::string lifetime;
		std::string reason;
	};
	std::string base, work, target, sibling, file;
	struct tool_context *context = nullptr;
	struct tool_registry registry = {};
	struct tool_result result = {};
	struct db database = {};
	std::vector<Approval> approvals;
	enum tool_operation_verdict verdict = TOOL_OP_ALLOW;

	static enum tool_operation_verdict approve(const struct tool_operation *op, void *data)
	{
		auto *self = static_cast<DirectoryPermissionTest *>(data);
		self->approvals.push_back({op->path_access, op->target ? op->target : "",
			op->scope ? op->scope : "", op->reason ? op->reason : ""});
		if (op->path_access) {
			if (op->path_access & (op->path_access - 1u))
				EXPECT_EQ(op->kind, TOOL_OP_DIRECTORY_ACCESS);
			cJSON *details = cJSON_Parse(op->details_json);
			EXPECT_NE(details, nullptr);
			if (details)
				EXPECT_STREQ(cJSON_GetObjectItem(details, "access")->valuestring,
					tool_path_access_name(op->path_access));
			cJSON_Delete(details);
		}
		return self->verdict;
	}

	void SetUp() override
	{
		const char *home = getenv("HOME");
		ASSERT_NE(home, nullptr);
		std::string pattern = std::string(home) + "/.morph_batch_permission_XXXXXX";
		std::vector<char> path(pattern.begin(), pattern.end());
		path.push_back('\0');
		ASSERT_NE(mkdtemp(path.data()), nullptr);
		base = path.data();
		work = base + "/work";
		target = base + "/target";
		sibling = base + "/sibling";
		file = target + "/file";
		std::filesystem::create_directories(work);
		std::filesystem::create_directories(target);
		std::filesystem::create_directories(sibling);
		ASSERT_EQ(file_write_all(file.c_str(), "data", 4), 0);
		context = tool_context_create(NULL, work.c_str(), work.c_str());
		ASSERT_NE(context, nullptr);
		tool_context_set_operation_approval(context, approve, this);
		tool_registry_init(NULL, &registry);
		ASSERT_EQ(request_permissions_init(&registry, context), 0);
		tool_result_init(&result);
	}

	void TearDown() override
	{
		tool_result_cleanup(&result);
		tool_registry_cleanup(&registry);
		tool_context_destroy(context);
		if (database.handle)
			db_close(&database);
		if (!base.empty())
			std::filesystem::remove_all(base);
	}

	std::string paths(const std::vector<std::string> &paths)
	{
		cJSON *array = cJSON_CreateArray();
		for (const auto &path : paths)
			cJSON_AddItemToArray(array, cJSON_CreateString(path.c_str()));
		char *encoded = cJSON_PrintUnformatted(array);
		std::string json = encoded;
		free(encoded);
		cJSON_Delete(array);
		return json;
	}

	std::string paths(const std::string &path)
	{
		return paths(std::vector<std::string>{path});
	}

	std::string all(const std::string &path)
	{
		return "{\"read\":" + paths(path) + ",\"write\":" + paths(path) +
			",\"delete\":" + paths(path) + "}";
	}

	void request(const std::string &permissions, const char *scope = "turn")
	{
		std::string json = "{\"permissions\":{\"file_system\":" + permissions +
			"},\"scope\":\"" + scope +
			"\",\"justification\":\"Maintain project files\"}";
		ASSERT_EQ(tool_exec(&registry, "request_permissions", json.c_str(), &result), 0);
		ASSERT_EQ(tool_result_finalize(&result), 0);
	}

	bool success()
	{
		return cJSON_IsTrue(cJSON_GetObjectItem(result.envelope, "ok"));
	}

	int access(enum tool_path_op operation, const std::string &path)
	{
		return tool_context_authorize_path(context, operation, path.c_str(), nullptr, 0);
	}
};

TEST_F(DirectoryPermissionTest, CanonicalDirectoryIsApprovedOnceForAllRequestedOperations)
{
	std::string alias = base + "/alias";
	ASSERT_EQ(symlink(target.c_str(), alias.c_str()), 0);
	request("{\"read\":" + paths(alias) + ",\"write\":" + paths(target + "/./") +
		",\"delete\":" + paths(target) + "}", "session");
	ASSERT_TRUE(success());
	ASSERT_EQ(approvals.size(), 1U);
	EXPECT_EQ(approvals[0].access, TOOL_PATH_ACCESS_ALL);
	EXPECT_EQ(approvals[0].lifetime, "session");
	EXPECT_EQ(approvals[0].reason, "Maintain project files");
	for (const char *key : {"read", "write", "delete"})
		EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(result.data, key)), 1);
	EXPECT_EQ(access(TOOL_PATH_READ, file), 0);
	EXPECT_EQ(access(TOOL_PATH_WRITE, file), 0);
	EXPECT_EQ(access(TOOL_PATH_DELETE, file), 0);
	request(all(alias), "session");
	EXPECT_TRUE(success());
	EXPECT_EQ(approvals.size(), 1U);
	const char *grants[2] = {};
	EXPECT_EQ(tool_context_collect_write_grants(context, "/bin/cp", grants, 2), 1);
	EXPECT_EQ(tool_context_collect_delete_grants(context, "/bin/rm", grants, 2), 1);
}

TEST_F(DirectoryPermissionTest, ExistingAccessPromptsOnlyForMissingOperations)
{
	request("{\"read\":" + paths(target) + "}", "session");
	ASSERT_TRUE(success());
	request(all(target), "session");
	ASSERT_TRUE(success());
	ASSERT_EQ(approvals.size(), 2U);
	EXPECT_EQ(approvals[0].access, TOOL_PATH_ACCESS_READ);
	EXPECT_EQ(approvals[1].access, TOOL_PATH_ACCESS_WRITE | TOOL_PATH_ACCESS_DELETE);
	request(all(target), "session");
	EXPECT_EQ(approvals.size(), 2U);
}

TEST_F(DirectoryPermissionTest, ReadOnlyApprovalDoesNotGrantWriteOrDelete)
{
	request("{\"read\":" + paths(target) + "}");
	ASSERT_TRUE(success());
	verdict = TOOL_OP_DENY;
	EXPECT_EQ(access(TOOL_PATH_READ, file), 0);
	EXPECT_EQ(access(TOOL_PATH_WRITE, file), -EACCES);
	EXPECT_EQ(access(TOOL_PATH_DELETE, file), -EACCES);
}

TEST_F(DirectoryPermissionTest, DenialGrantsNothingAndCreatesNoDirectories)
{
	std::string missing = base + "/missing";
	verdict = TOOL_OP_DENY;
	request(all(missing));
	EXPECT_FALSE(success());
	ASSERT_EQ(approvals.size(), 1U);
	EXPECT_FALSE(std::filesystem::exists(missing));
	verdict = TOOL_OP_ALLOW;
	request(all(missing));
	ASSERT_TRUE(success());
	EXPECT_TRUE(std::filesystem::is_directory(missing));
	EXPECT_EQ(approvals.size(), 2U);
}

TEST_F(DirectoryPermissionTest, InvalidLaterInputNeverApprovesEarlierDirectories)
{
	request("{\"read\":" + paths(target) + ",\"delete\":[42]}");
	EXPECT_FALSE(success());
	EXPECT_TRUE(approvals.empty());
	request("{\"write\":" + paths(target) + ",\"delete\":" + paths(file) + "}");
	EXPECT_FALSE(success());
	EXPECT_TRUE(approvals.empty());
	request("{\"read\":" + paths(base + "/missing") + "}");
	EXPECT_FALSE(success());
	EXPECT_TRUE(approvals.empty());
}

TEST_F(DirectoryPermissionTest, ParentGrantAvoidsRedundantChildApprovalWithoutBroadeningChildRights)
{
	std::string child = target + "/child";
	std::filesystem::create_directory(child);
	request("{\"read\":" + paths(std::vector<std::string>{child, target}) +
		",\"write\":" + paths(target) + ",\"delete\":" + paths(target) + "}");
	ASSERT_TRUE(success());
	EXPECT_EQ(approvals.size(), 1U);
	verdict = TOOL_OP_DENY;
	EXPECT_EQ(access(TOOL_PATH_WRITE, sibling + "/file"), -EACCES);
}

TEST_F(DirectoryPermissionTest, TurnGrantsExpireAndSessionUpgradeRequiresOneCombinedApproval)
{
	request(all(target));
	ASSERT_TRUE(success());
	tool_context_clear_turn_grants(context);
	verdict = TOOL_OP_DENY;
	request(all(target));
	EXPECT_FALSE(success());
	ASSERT_EQ(approvals.size(), 2U);
	verdict = TOOL_OP_ALLOW;
	request(all(target));
	ASSERT_TRUE(success());
	request(all(target), "session");
	ASSERT_TRUE(success());
	ASSERT_EQ(approvals.size(), 4U);
	EXPECT_EQ(approvals.back().access, TOOL_PATH_ACCESS_ALL);
	tool_context_clear_turn_grants(context);
	verdict = TOOL_OP_DENY;
	request(all(target), "session");
	EXPECT_TRUE(success());
	EXPECT_EQ(approvals.size(), 4U);
}

TEST_F(DirectoryPermissionTest, SessionChoiceKeepsAllGrantedOperationsAcrossTurns)
{
	verdict = TOOL_OP_SESSION;
	request(all(target));
	ASSERT_TRUE(success());
	tool_context_clear_turn_grants(context);
	verdict = TOOL_OP_DENY;
	request(all(target));
	EXPECT_TRUE(success());
	EXPECT_EQ(approvals.size(), 1U);
}

TEST_F(DirectoryPermissionTest, AlwaysPersistsTheWholeApprovedBatch)
{
	ASSERT_EQ(db_open(NULL, &database, ":memory:"), 0);
	ASSERT_EQ(db_init_schema(&database), 0);
	ASSERT_EQ(tool_context_set_grant_store(context, &database, work.c_str()), 0);
	verdict = TOOL_OP_ALWAYS;
	request(all(target));
	ASSERT_TRUE(success());
	ASSERT_EQ(approvals.size(), 1U);
	tool_context_clear_turn_grants(context);
	ASSERT_EQ(tool_context_reload_persistent_grants(context), 0);
	verdict = TOOL_OP_DENY;
	request(all(target), "session");
	EXPECT_TRUE(success());
	EXPECT_EQ(approvals.size(), 1U);
	EXPECT_EQ(access(TOOL_PATH_READ, file), 0);
	EXPECT_EQ(access(TOOL_PATH_WRITE, file), 0);
	EXPECT_EQ(access(TOOL_PATH_DELETE, file), 0);
}

TEST_F(DirectoryPermissionTest, PersistenceFailureRollsBackEveryOperation)
{
	ASSERT_EQ(db_open(NULL, &database, ":memory:"), 0);
	ASSERT_EQ(db_init_schema(&database), 0);
	ASSERT_EQ(tool_context_set_grant_store(context, &database, work.c_str()), 0);
	ASSERT_EQ(db_exec(&database,
		"CREATE TRIGGER reject_write BEFORE INSERT ON permission_grants "
		"WHEN NEW.resource_kind='write_path' BEGIN SELECT RAISE(ABORT,'reject'); END;"), 0);
	verdict = TOOL_OP_ALWAYS;
	request(all(target));
	EXPECT_FALSE(success());
	verdict = TOOL_OP_DENY;
	EXPECT_EQ(access(TOOL_PATH_READ, file), -EACCES);
	EXPECT_EQ(access(TOOL_PATH_WRITE, file), -EACCES);
	EXPECT_EQ(access(TOOL_PATH_DELETE, file), -EACCES);
	ASSERT_EQ(tool_context_reload_persistent_grants(context), 0);
	EXPECT_EQ(access(TOOL_PATH_READ, file), -EACCES);
}

TEST_F(DirectoryPermissionTest, ChildOnlyWriteDoesNotExtendToItsParent)
{
	std::string child = target + "/child";
	std::filesystem::create_directory(child);
	request("{\"read\":" + paths(target) + ",\"write\":" + paths(child) + "}");
	ASSERT_TRUE(success());
	ASSERT_EQ(approvals.size(), 2U);
	EXPECT_EQ(approvals[0].access, TOOL_PATH_ACCESS_READ);
	EXPECT_EQ(approvals[1].access, TOOL_PATH_ACCESS_WRITE);
	verdict = TOOL_OP_DENY;
	EXPECT_EQ(access(TOOL_PATH_WRITE, child + "/file"), 0);
	EXPECT_EQ(access(TOOL_PATH_WRITE, file), -EACCES);
	EXPECT_EQ(access(TOOL_PATH_DELETE, child + "/file"), -EACCES);
}

TEST_F(DirectoryPermissionTest, CombinedApprovalIsNotDisguisedAsALegacyDeleteOperation)
{
	tool_context_set_operation_approval(context,
		[](const struct tool_operation *operation, void *) {
			return operation->kind == TOOL_OP_PATH_DELETE ?
				TOOL_OP_ALLOW : TOOL_OP_DENY;
		}, nullptr);
	request(all(target));
	EXPECT_FALSE(success());
	request("{\"delete\":" + paths(target) + "}");
	EXPECT_TRUE(success());
	EXPECT_EQ(access(TOOL_PATH_READ, file), -EACCES);
	EXPECT_EQ(access(TOOL_PATH_WRITE, file), -EACCES);
	EXPECT_EQ(access(TOOL_PATH_DELETE, file), 0);
}
