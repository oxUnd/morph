#include <gtest/gtest.h>

extern "C" {
#include "agent/patch.h"
#include "agent/tool.h"
#include "agent/tool_context.h"
#include "agent/tools/apply_patch.h"
#include "agent/tools/checkpoint.h"
#include "agent/tool_runtime.h"
#include "util/cleanup.h"
#include "util/buf.h"
#include "util/file.h"
}

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

class PatchTest : public ::testing::Test {
protected:
	char dir[PATH_MAX]{};

	void SetUp() override
	{
		std::strcpy(dir, "/tmp/morph-patch-test-XXXXXX");
		ASSERT_NE(mkdtemp(dir), nullptr);
	}

	void TearDown() override
	{
		std::error_code error;

		std::filesystem::remove_all(dir, error);
	}

	std::string path(const char *name) const
	{
		return std::string(dir) + "/" + name;
	}

	void write_file(const char *name, const char *content)
	{
		ASSERT_EQ(file_write_all(path(name).c_str(), content,
			std::strlen(content)), 0);
	}

	std::string read_file(const char *name)
	{
		char *content = file_read_all_copy(path(name).c_str(), nullptr);
		EXPECT_NE(content, nullptr);
		std::string value = content ? content : "";
		std::free(content);
		return value;
	}

	int apply(const char *input, struct patch_result *result,
		  char *error, size_t error_size)
	{
		return patch_apply(dir, input, result, error, error_size);
	}

	std::string preview(const char *input, int *rc, char *error,
			    size_t error_size)
	{
		morph_buf_t out;
		std::string text;

		if (morph_buf_init(&out, 256) != 0)
			return text;
		*rc = patch_preview(dir, input, &out, error, error_size);
		if (*rc == 0)
			text = morph_buf_cstr_borrow(&out);
		morph_buf_cleanup(&out);
		return text;
	}
};

struct PatchTrackingRecorder {
	bool active = false;
	int begin_count = 0;
	int finish_count = 0;
	int begin_error = 0;
	int finish_error = 0;
	int request_error = 0;
	int request_count = 0;
	int64_t requested_id = 0;
	int validation_error = 0;
	int validation_count = 0;
	size_t validated_bytes = 0;
	std::string validated_target;
	std::string validated_removed;
	std::string requested_action;
	std::string fail_capture;
	std::vector<std::string> paths;
	std::map<std::string, std::string> before;
	std::map<std::string, std::string> after;
};

static std::string tracking_file_contents(const char *path)
{
	if (!std::filesystem::exists(path))
		return "<absent>";
	std::ifstream file(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

static int tracking_begin(void *opaque, int64_t id)
{
	auto &recorder = *static_cast<PatchTrackingRecorder *>(opaque);
	EXPECT_EQ(id, 42);
	EXPECT_FALSE(recorder.active);
	recorder.begin_count++;
	if (recorder.begin_error)
		return recorder.begin_error;
	recorder.active = true;
	return 0;
}

static int tracking_capture(void *opaque, int64_t id, const char *path)
{
	auto &recorder = *static_cast<PatchTrackingRecorder *>(opaque);
	const std::string name = std::filesystem::path(path).filename();
	EXPECT_EQ(id, 42);
	EXPECT_TRUE(recorder.active);
	if (name == recorder.fail_capture)
		return -EIO;
	recorder.paths.emplace_back(path);
	recorder.before[name] = tracking_file_contents(path);
	return 0;
}

static int tracking_finish(void *opaque, int64_t id)
{
	auto &recorder = *static_cast<PatchTrackingRecorder *>(opaque);
	EXPECT_EQ(id, 42);
	EXPECT_TRUE(recorder.active);
	for (const auto &path : recorder.paths)
		recorder.after[std::filesystem::path(path).filename()] =
			tracking_file_contents(path.c_str());
	recorder.finish_count++;
	recorder.active = false;
	return recorder.finish_error;
}

static int tracking_validate_write(void *opaque, int64_t id, const char *path, size_t bytes,
				   const char *removed_path)
{
	auto &recorder = *static_cast<PatchTrackingRecorder *>(opaque);

	EXPECT_EQ(id, 42);
	EXPECT_TRUE(recorder.active);
	recorder.validation_count++;
	recorder.validated_target = std::filesystem::path(path).filename();
	recorder.validated_bytes = bytes;
	recorder.validated_removed = removed_path ?
		std::filesystem::path(removed_path).filename().string() : "";
	return recorder.validation_error;
}

static int tracking_request(void *opaque, const char *action, int64_t id, cJSON **out)
{
	auto &recorder = *static_cast<PatchTrackingRecorder *>(opaque);
	EXPECT_EQ(*out, nullptr);
	recorder.request_count++;
	recorder.requested_action = action;
	recorder.requested_id = id;
	*out = cJSON_CreateObject();
	if (!*out)
		return -ENOMEM;
	cJSON_AddStringToObject(*out, "action", action);
	cJSON_AddNumberToObject(*out, "id", static_cast<double>(id));
	cJSON_AddBoolToObject(*out, "restored", 0);
	cJSON *conflicts = cJSON_AddArrayToObject(*out, "conflicts");
	if (recorder.request_error)
		cJSON_AddItemToArray(conflicts, cJSON_CreateString("later-user-edit.txt"));
	return recorder.request_error;
}

class PatchTrackingTest : public PatchTest
{
      protected:
	PatchTrackingRecorder recorder;
	struct tool_registry registry{};
	struct tool_context *context = nullptr;
	struct tool_runtime_context runtime{};

	void SetUp() override
	{
		PatchTest::SetUp();
		tool_registry_init(nullptr, &registry);
		context = tool_context_create(nullptr, dir, dir);
		ASSERT_NE(context, nullptr);
		ASSERT_EQ(apply_patch_init(&registry, context), 0);
		ASSERT_EQ(checkpoint_init(&registry), 0);
		runtime.checkpoint = {42,
				      &recorder,
				      tracking_begin,
				      tracking_capture,
				      tracking_finish,
				      tracking_request,
				      0,
				      nullptr};
	}

	void TearDown() override
	{
		tool_registry_cleanup(&registry);
		tool_context_destroy(context);
		PatchTest::TearDown();
	}

	int run(const char *tool, const char *input, struct tool_result *result)
	{
		result->runtime = &runtime;
		return tool_exec(&registry, tool, input, result);
	}
};

TEST_F(PatchTrackingTest, CapturesEveryPreimageBeforeEditsIncludingMoveDestination)
{
	write_file("first.txt", "old first\n");
	write_file("source.txt", "old source\n");
	write_file("destination.txt", "user destination\n");
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	ASSERT_EQ(run("apply_patch",
		      "*** Begin Patch\n*** Update File: first.txt\n@@\n-old first\n+new first\n"
		      "*** Update File: source.txt\n*** Move to: destination.txt\n@@\n"
		      "-old source\n+new source\n*** End Patch",
		      &result),
		  0);
	EXPECT_EQ(recorder.before["first.txt"], "old first\n");
	EXPECT_EQ(recorder.before["source.txt"], "old source\n");
	EXPECT_EQ(recorder.before["destination.txt"], "user destination\n");
	EXPECT_EQ(recorder.after["first.txt"], "new first\n");
	EXPECT_EQ(recorder.after["source.txt"], "<absent>");
	EXPECT_EQ(recorder.after["destination.txt"], "new source\n");
	EXPECT_EQ(recorder.finish_count, 1);
	EXPECT_FALSE(recorder.active);
	const cJSON *checkpoint = cJSON_GetObjectItem(result.data, "checkpoint");
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(checkpoint, "id")), 42);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(checkpoint, "status")), "sealed");
}

TEST_F(PatchTrackingTest, BeginOrCaptureFailureNeverMutatesFiles)
{
	write_file("first.txt", "first\n");
	write_file("second.txt", "second\n");
	const char *patch = "*** Begin Patch\n*** Update File: first.txt\n@@\n-first\n+changed\n"
			    "*** Update File: second.txt\n@@\n-second\n+changed\n*** End Patch";
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	recorder.begin_error = -EIO;
	EXPECT_EQ(run("apply_patch", patch, &result), -EIO);
	EXPECT_EQ(recorder.finish_count, 0);
	EXPECT_EQ(read_file("first.txt"), "first\n");
	recorder.begin_error = 0;
	recorder.fail_capture = "second.txt";
	EXPECT_EQ(run("apply_patch", patch, &result), -EIO);
	EXPECT_EQ(recorder.finish_count, 1);
	EXPECT_FALSE(recorder.active);
	EXPECT_EQ(read_file("first.txt"), "first\n");
	EXPECT_EQ(read_file("second.txt"), "second\n");
	const cJSON *checkpoint = cJSON_GetObjectItem(result.meta, "checkpoint");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(checkpoint, "status")),
		     "capture_failed");
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(checkpoint, "changes_may_have_applied")));
	runtime.checkpoint.capture = nullptr;
	EXPECT_EQ(run("apply_patch", patch, &result), MORPH_ERR_NOT_CONFIGURED);
	EXPECT_EQ(recorder.begin_count, 2);
	EXPECT_EQ(recorder.finish_count, 1);
	EXPECT_EQ(read_file("first.txt"), "first\n");
}

TEST_F(PatchTrackingTest, ParseFailureAndPermissionDenialStillFinishWithoutCapture)
{
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	EXPECT_LT(run("apply_patch", "invalid patch", &result), 0);
	EXPECT_EQ(recorder.finish_count, 1);
	EXPECT_TRUE(recorder.paths.empty());
	struct permission_policy policy = permission_policy_default();
	policy.access = PERMISSION_READ_ONLY;
	policy.approval = PERMISSION_NEVER;
	ASSERT_EQ(tool_context_set_permission_policy(context, &policy), 0);
	EXPECT_LT(run("apply_patch", "*** Begin Patch\n*** Add File: denied.txt\n+x\n*** End Patch",
		      &result),
		  0);
	EXPECT_EQ(recorder.finish_count, 2);
	EXPECT_TRUE(recorder.paths.empty());
	EXPECT_FALSE(std::filesystem::exists(path("denied.txt")));
}

TEST_F(PatchTrackingTest, PartialPatchFailureSealsChangesAndReportsRisk)
{
	write_file("first.txt", "before\n");
	write_file("second.txt", "actual\n");
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	EXPECT_LT(run("apply_patch",
		      "*** Begin Patch\n*** Update File: first.txt\n@@\n-before\n+after\n"
		      "*** Update File: second.txt\n@@\n-missing\n+changed\n*** End Patch",
		      &result),
		  0);
	EXPECT_EQ(recorder.after["first.txt"], "after\n");
	EXPECT_EQ(recorder.after["second.txt"], "actual\n");
	EXPECT_EQ(recorder.finish_count, 1);
	const cJSON *checkpoint = cJSON_GetObjectItem(result.meta, "checkpoint");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(checkpoint, "status")), "sealed");
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(checkpoint, "changes_may_have_applied")));
	EXPECT_NE(strstr(result.text.data, "Files may already have changed"), nullptr);
}

TEST_F(PatchTrackingTest, SealFailureAfterEditingIsNeverReportedAsSuccess)
{
	recorder.finish_error = -EIO;
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	EXPECT_EQ(run("apply_patch",
		      "*** Begin Patch\n*** Add File: changed.txt\n+created\n*** End Patch",
		      &result),
		  -EIO);
	EXPECT_EQ(read_file("changed.txt"), "created\n");
	EXPECT_EQ(recorder.finish_count, 1);
	EXPECT_FALSE(recorder.active);
	EXPECT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(result.envelope, "ok")));
	const cJSON *checkpoint = cJSON_GetObjectItem(result.meta, "checkpoint");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(checkpoint, "status")),
		     "seal_failed");
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(checkpoint, "changes_may_have_applied")));
}

TEST_F(PatchTrackingTest, RejectsOversizedPostimageBeforeWritingAndStillSeals)
{
	runtime.checkpoint.max_file_bytes = 8;
	write_file("file.txt", "ok\n");
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	EXPECT_EQ(run("apply_patch",
		"*** Begin Patch\n*** Update File: file.txt\n@@\n-ok\n+too much content\n"
		"*** End Patch", &result), -EFBIG);
	EXPECT_EQ(read_file("file.txt"), "ok\n");
	EXPECT_EQ(recorder.finish_count, 1);
	EXPECT_NE(std::string(result.text.data).find("8-byte checkpoint limit"), std::string::npos);
	tool_result_cleanup(&result);
	EXPECT_EQ(run("apply_patch",
		"*** Begin Patch\n*** Add File: new.txt\n+too much content\n*** End Patch",
		&result),
		-EFBIG);
	EXPECT_FALSE(std::filesystem::exists(path("new.txt")));
	EXPECT_EQ(recorder.finish_count, 2);
}

TEST_F(PatchTrackingTest, AggregateValidationRejectsAWriteAndStillSealsTheAttempt)
{
	runtime.checkpoint.validate_write = tracking_validate_write;
	recorder.validation_error = -EFBIG;
	write_file("file.txt", "before\n");
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	EXPECT_EQ(run("apply_patch",
		"*** Begin Patch\n*** Update File: file.txt\n@@\n-before\n+after\n*** End Patch",
		&result), -EFBIG);
	EXPECT_EQ(read_file("file.txt"), "before\n");
	EXPECT_EQ(recorder.validation_count, 1);
	EXPECT_EQ(recorder.validated_bytes, 6u);
	EXPECT_EQ(recorder.validated_target, "file.txt");
	EXPECT_EQ(recorder.validated_removed, "");
	EXPECT_EQ(recorder.finish_count, 1);
	EXPECT_NE(std::string(result.text.data).find("postimage budget rejected"),
		std::string::npos);
}

TEST_F(PatchTrackingTest, MoveValidationCombinesTheDestinationWriteAndSourceRemoval)
{
	runtime.checkpoint.validate_write = tracking_validate_write;
	write_file("old.txt", "old\n");
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	ASSERT_EQ(run("apply_patch",
		"*** Begin Patch\n*** Update File: old.txt\n*** Move to: new.txt\n@@\n-old\n+new\n"
		"*** End Patch", &result), 0);
	EXPECT_EQ(recorder.validation_count, 1);
	EXPECT_EQ(recorder.validated_target, "new.txt");
	EXPECT_EQ(recorder.validated_removed, "old.txt");
	EXPECT_EQ(recorder.validated_bytes, 4u);
	EXPECT_FALSE(std::filesystem::exists(path("old.txt")));
	EXPECT_EQ(read_file("new.txt"), "new\n");
}

TEST_F(PatchTrackingTest, CheckpointRequiresExplicitTargetAndRetainsConflictDetails)
{
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	ASSERT_EQ(run("checkpoint", "{\"action\":\"list\"}", &result), 0);
	EXPECT_EQ(recorder.requested_id, 0);
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(result.data, "current_checkpoint_id")), 42);
	for (const char *input :
	     {"{\"action\":\"preview\"}", "{\"action\":\"restore_code\"}",
	      "{\"action\":\"preview\",\"id\":1.5}", "{\"action\":\"restore_code\",\"id\":0}",
	      "{\"action\":\"preview\",\"id\":1e30}", "{\"action\":\"rewind\",\"id\":42}"})
		EXPECT_EQ(run("checkpoint", input, &result), -EINVAL);
	EXPECT_EQ(recorder.request_count, 1);
	ASSERT_EQ(run("checkpoint", "{\"action\":\"preview\",\"id\":42}", &result), 0);
	EXPECT_EQ(recorder.requested_action, "preview");
	EXPECT_EQ(recorder.requested_id, 42);
	recorder.request_error = -EAGAIN;
	EXPECT_EQ(run("checkpoint", "{\"action\":\"restore_code\",\"id\":42}", &result), -EAGAIN);
	const cJSON *checkpoint = cJSON_GetObjectItem(result.meta, "checkpoint");
	EXPECT_EQ(cJSON_GetArraySize(cJSON_GetObjectItem(checkpoint, "conflicts")), 1);
}

TEST_F(PatchTrackingTest, SubagentEditsKeepLegacyBehaviorAndCannotRequestRestore)
{
	runtime.sub_agent_depth = 1;
	struct tool_result result MORPH_CLEANUP(tool_result_cleanup) = {};
	ASSERT_EQ(run("apply_patch",
		      "*** Begin Patch\n*** Add File: child.txt\n+child\n*** End Patch", &result),
		  0);
	EXPECT_EQ(read_file("child.txt"), "child\n");
	EXPECT_EQ(recorder.begin_count, 0);
	EXPECT_EQ(recorder.finish_count, 0);
	EXPECT_EQ(run("checkpoint", "{\"action\":\"restore_code\",\"id\":42}", &result), -EPERM);
	EXPECT_EQ(recorder.request_count, 0);
}

TEST_F(PatchTest, AddsUpdatesAndDeletesFiles)
{
	write_file("update.txt", "alpha\nbeta\n");
	write_file("delete.txt", "gone\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Add File: add.txt\n"
		"+created\n"
		"*** Update File: update.txt\n"
		"@@\n"
		" alpha\n"
		"-beta\n"
		"+gamma\n"
		"*** Delete File: delete.txt\n"
		"*** End Patch\n";
	struct patch_result result{};
	char error[512];

	ASSERT_EQ(apply(input, &result, error, sizeof(error)), 0) << error;
	EXPECT_EQ(result.changes.nelts, 3U);
	EXPECT_EQ(read_file("add.txt"), "created\n");
	EXPECT_EQ(read_file("update.txt"), "alpha\ngamma\n");
	EXPECT_EQ(access(path("delete.txt").c_str(), F_OK), -1);
	patch_result_cleanup(&result);
}

TEST_F(PatchTest, AppliesMultipleOrderedHunks)
{
	write_file("sample.txt", "one\ntwo\nthree\nfour\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: sample.txt\n"
		"@@\n"
		" one\n"
		"-two\n"
		"+second\n"
		"@@\n"
		" three\n"
		"-four\n"
		"+fourth\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	ASSERT_EQ(apply(input, &result, error, sizeof(error)), 0) << error;
	EXPECT_EQ(read_file("sample.txt"), "one\nsecond\nthree\nfourth\n");
	patch_result_cleanup(&result);
}

TEST_F(PatchTest, UsesSemanticHunkContextLikeCodex)
{
	write_file("numbered.txt", "one\ntwo\nthree\n");
	const char *valid =
		"*** Begin Patch\n"
		"*** Update File: numbered.txt\n"
		"@@ one\n"
		"-two\n"
		"+second\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	ASSERT_EQ(apply(valid, &result, error, sizeof(error)), 0) << error;
	EXPECT_EQ(read_file("numbered.txt"), "one\nsecond\nthree\n");
	patch_result_cleanup(&result);

	const char *invalid =
		"*** Begin Patch\n"
		"*** Update File: numbered.txt\n"
		"@@ absent anchor\n"
		"-missing\n"
		"+other\n"
		"*** End Patch";
	EXPECT_EQ(apply(invalid, &result, error, sizeof(error)), -EINVAL);
	EXPECT_NE(std::strstr(error, "expected lines"), nullptr);
	EXPECT_NE(std::strstr(error, "patch line 3"), nullptr);
}

TEST_F(PatchTest, MissingFileErrorIncludesResolvedWorkspacePath)
{
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: missing.txt\n"
		"@@ -1,1 +1,1 @@\n"
		"-old\n"
		"+new\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	EXPECT_EQ(apply(input, &result, error, sizeof(error)), -ENOENT);
	EXPECT_NE(std::strstr(error, dir), nullptr);
	EXPECT_NE(std::strstr(error, "missing.txt"), nullptr);
}

TEST_F(PatchTest, AppliesSingleUpdateHunkWithoutHeader)
{
	write_file("todo.c",
		"    unsigned int max_id = 0;\n"
		"    Todo t;\n"
		"    long pos = ftell(f);\n"
		"    while (read_record(f, &t) == 1) {\n"
		"        if (t.id > max_id) max_id = t.id;\n"
		"    }\n"
		"    fseek(f, pos, SEEK_SET); /* 回到记录起点, 追加 */\n"
		"    t.id = max_id + 1;\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: todo.c\n"
		"    unsigned int max_id = 0;\n"
		"    Todo t;\n"
		"    long pos = ftell(f);\n"
		"    while (read_record(f, &t) == 1) {\n"
		"        if (t.id > max_id) max_id = t.id;\n"
		"    }\n"
		"-    fseek(f, pos, SEEK_SET); /* 回到记录起点, 追加 */\n"
		"+    fseek(f, 0, SEEK_END); /* 追加到文件末尾 */\n"
		"    t.id = max_id + 1;\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	ASSERT_EQ(apply(input, &result, error, sizeof(error)), 0) << error;
	EXPECT_NE(read_file("todo.c").find("fseek(f, 0, SEEK_END)"),
		  std::string::npos);
	EXPECT_EQ(result.changes.nelts, 1U);
	patch_result_cleanup(&result);
}

TEST_F(PatchTest, FailureAfterSuccessKeepsEarlierChangesLikeCodex)
{
	write_file("first.txt", "before\n");
	write_file("second.txt", "actual\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: first.txt\n"
		"@@\n"
		"-before\n"
		"+after\n"
		"*** Update File: second.txt\n"
		"@@\n"
		"-missing\n"
		"+changed\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	EXPECT_EQ(apply(input, &result, error, sizeof(error)), -EINVAL);
	EXPECT_NE(std::strstr(error, "expected lines"), nullptr);
	EXPECT_EQ(read_file("first.txt"), "after\n");
	EXPECT_EQ(read_file("second.txt"), "actual\n");
}

TEST_F(PatchTest, AppliesFirstMatchingContextLikeCodex)
{
	write_file("repeat.txt", "same\nsame\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: repeat.txt\n"
		"@@\n"
		"-same\n"
		"+other\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	ASSERT_EQ(apply(input, &result, error, sizeof(error)), 0) << error;
	EXPECT_EQ(read_file("repeat.txt"), "other\nsame\n");
	patch_result_cleanup(&result);
}

TEST_F(PatchTest, MatchesIgnoringSurroundingWhitespaceLikeCodex)
{
	write_file("space.txt", "    old value   \nnext\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: space.txt\n"
		"@@\n"
		"-old value\n"
		"+new value\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	ASSERT_EQ(apply(input, &result, error, sizeof(error)), 0) << error;
	EXPECT_EQ(read_file("space.txt"), "new value\nnext\n");
	patch_result_cleanup(&result);
}

TEST_F(PatchTest, RejectsTraversalAndAbsolutePaths)
{
	const char *traversal =
		"*** Begin Patch\n*** Add File: ../escape.txt\n+x\n*** End Patch";
	const char *absolute =
		"*** Begin Patch\n*** Add File: /tmp/escape.txt\n+x\n*** End Patch";
	struct patch_result result{};
	char error[512];

	EXPECT_EQ(apply(traversal, &result, error, sizeof(error)), -EINVAL);
	EXPECT_EQ(apply(absolute, &result, error, sizeof(error)), -EINVAL);
}

TEST_F(PatchTest, AddFileErrorReportsPatchLine)
{
	const char *input =
		"*** Begin Patch\n"
		"*** Add File: broken.txt\n"
		"+valid\n"
		"missing prefix\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	EXPECT_EQ(apply(input, &result, error, sizeof(error)), -EINVAL);
	EXPECT_STREQ(error, "invalid add-file line 4 in broken.txt: "
		     "content lines must start with '+'");
}

TEST_F(PatchTest, RejectsSymbolicLinkTargets)
{
	write_file("real.txt", "value\n");
	ASSERT_EQ(symlink(path("real.txt").c_str(), path("link.txt").c_str()), 0);
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: link.txt\n"
		"@@\n"
		"-value\n"
		"+changed\n"
		"*** End Patch";
	struct patch_result result{};
	char error[512];

	EXPECT_EQ(apply(input, &result, error, sizeof(error)), -ELOOP);
	EXPECT_EQ(read_file("real.txt"), "value\n");
}

TEST_F(PatchTest, PreservesUpdatedFileMode)
{
	write_file("script.sh", "old\n");
	ASSERT_EQ(chmod(path("script.sh").c_str(), 0751), 0);
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: script.sh\n"
		"@@\n"
		"-old\n"
		"+new\n"
		"*** End Patch";
	struct patch_result result{};
	struct stat st{};
	char error[512];

	ASSERT_EQ(apply(input, &result, error, sizeof(error)), 0) << error;
	ASSERT_EQ(stat(path("script.sh").c_str(), &st), 0);
	EXPECT_EQ(st.st_mode & 0777, 0751);
	patch_result_cleanup(&result);
}

TEST_F(PatchTest, ToolAcceptsRawTextAndPublishesGrammar)
{
	struct tool_registry registry{};
	struct tool_context *context = tool_context_create(NULL, dir, dir);
	struct tool_result result{};
	const char *input =
		"*** Begin Patch\n"
		"*** Add File: tool.txt\n"
		"+raw input\n"
		"*** End Patch";

	ASSERT_NE(context, nullptr);
	tool_registry_init(NULL, &registry);
	ASSERT_EQ(apply_patch_init(&registry, context), 0);
	struct tool_entry *entry = tool_borrow(&registry, "apply_patch");
	ASSERT_NE(entry, nullptr);
	EXPECT_EQ(entry->desc.input_kind, TOOL_INPUT_TEXT);
	EXPECT_NE(entry->flags & TOOL_FLAG_INTERNAL_APPROVAL, 0);
	EXPECT_NE(strstr(entry->desc.description, "up to 64 KiB"), nullptr);
	EXPECT_NE(strstr(entry->desc.description, "natural file or hunk boundaries"),
		nullptr);
	EXPECT_NE(strstr(entry->desc.description, "Codex patch"), nullptr);
	EXPECT_NE(strstr(entry->desc.description, dir), nullptr);
	EXPECT_NE(strstr(entry->desc.description, "copied verbatim"), nullptr);
	cJSON *format = cJSON_Parse(entry->desc.input_format);
	ASSERT_NE(format, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(format, "syntax")),
		"lark");
	cJSON_Delete(format);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&registry, "apply_patch", input, &result), 0);
	EXPECT_EQ(read_file("tool.txt"), "raw input\n");
	ASSERT_NE(result.envelope, nullptr);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(result.envelope, "ok")));
	tool_result_cleanup(&result);
	tool_registry_cleanup(&registry);
	tool_context_destroy(context);
}

TEST_F(PatchTest, PreviewRendersUnifiedDiffWithFileLineNumbers)
{
	write_file("multi.txt", "foo\nbar\nbaz\nqux\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: multi.txt\n"
		"@@\n"
		" foo\n"
		"-bar\n"
		"+BAR\n"
		"@@\n"
		" baz\n"
		"-qux\n"
		"+QUX\n"
		"*** End Patch";
	char error[512];
	int rc = 0;
	std::string diff = preview(input, &rc, error, sizeof(error));

	ASSERT_EQ(rc, 0) << error;
	EXPECT_EQ(diff,
		"*** Update File: multi.txt\n"
		"@@ -1,4 +1,4 @@\n"
		" foo\n"
		"-bar\n"
		"+BAR\n"
		" baz\n"
		"-qux\n"
		"+QUX\n");
	EXPECT_EQ(read_file("multi.txt"), "foo\nbar\nbaz\nqux\n");
}

TEST_F(PatchTest, PreviewKeepsContextInsideReplacedBlock)
{
	write_file("first.txt", "foo\nbar\nbaz\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: first.txt\n"
		"@@\n"
		"-foo\n"
		"+FOO\n"
		" bar\n"
		"*** End Patch";
	char error[512];
	int rc = 0;
	std::string diff = preview(input, &rc, error, sizeof(error));

	ASSERT_EQ(rc, 0) << error;
	EXPECT_EQ(diff,
		"*** Update File: first.txt\n"
		"@@ -1,2 +1,2 @@\n"
		"-foo\n"
		"+FOO\n"
		" bar\n");
}

TEST_F(PatchTest, PreviewRendersAddAndDeleteFiles)
{
	write_file("gone.txt", "a\nb\n");
	const char *add =
		"*** Begin Patch\n"
		"*** Add File: new.txt\n"
		"+one\n"
		"+two\n"
		"*** End Patch";
	const char *del =
		"*** Begin Patch\n"
		"*** Delete File: gone.txt\n"
		"*** End Patch";
	char error[512];
	int rc = 0;

	EXPECT_EQ(preview(add, &rc, error, sizeof(error)),
		"*** Add File: new.txt\n"
		"@@ -0,0 +1,2 @@\n"
		"+one\n"
		"+two\n");
	EXPECT_EQ(rc, 0) << error;
	EXPECT_EQ(preview(del, &rc, error, sizeof(error)),
		"*** Delete File: gone.txt\n"
		"@@ -1,2 +0,0 @@\n"
		"-a\n"
		"-b\n");
	EXPECT_EQ(rc, 0) << error;
	EXPECT_EQ(read_file("gone.txt"), "a\nb\n");
}

TEST_F(PatchTest, PreviewFailsWhenContextIsMissing)
{
	write_file("target.txt", "one\ntwo\n");
	const char *input =
		"*** Begin Patch\n"
		"*** Update File: target.txt\n"
		"@@\n"
		"-absent\n"
		"+present\n"
		"*** End Patch";
	char error[512];
	int rc = 0;

	EXPECT_EQ(preview(input, &rc, error, sizeof(error)), "");
	EXPECT_EQ(rc, -EINVAL);
	EXPECT_NE(std::strstr(error, "failed to find expected lines"), nullptr);
	EXPECT_EQ(read_file("target.txt"), "one\ntwo\n");
}
