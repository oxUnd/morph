#include "file_write.h"

#include "agent/tool_context.h"
#include "cJSON.h"
#include "util/file.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void expect_true(int *failures, int cond, const char *msg)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", msg);
		(*failures)++;
	}
}

static char *read_file(const char *path)
{
	size_t len;

	return file_read_all(path, &len);
}

static int run_tool(struct tool_registry *reg, const char *json,
		    struct tool_result *result)
{
	tool_result_clear(result);
	return tool_exec(reg, "file_write", json, result);
}

static int result_ok(struct tool_result *result)
{
	cJSON *root;
	cJSON *ok;
	int value;

	root = cJSON_Parse(result->text.data ? result->text.data : "");
	if (!root)
		return 0;
	ok = cJSON_GetObjectItem(root, "ok");
	value = cJSON_IsTrue(ok) ? 1 : 0;
	cJSON_Delete(root);
	return value;
}

static int result_code(struct tool_result *result)
{
	cJSON *root;
	cJSON *code;
	int value;

	root = cJSON_Parse(result->text.data ? result->text.data : "");
	if (!root)
		return 0;
	cJSON *error = cJSON_GetObjectItem(root, "error");
	cJSON *details = cJSON_GetObjectItem(error, "details");
	code = cJSON_GetObjectItem(details, "code");
	value = cJSON_IsNumber(code) ? (int)code->valuedouble : 0;
	cJSON_Delete(root);
	return value;
}

static void setup_registry(int *failures, struct tool_registry *reg, struct tool_context **tctx,
			   const char *work, const char *out)
{
	tool_registry_init(NULL, reg);
	*tctx = tool_context_create(NULL, work, out);
	expect_true(failures, *tctx != NULL, "tool_context_create");
	expect_true(failures, file_write_tool_init(reg, *tctx) == 0, "file_write_tool_init");
}

static void cleanup_registry(struct tool_registry *reg,
			     struct tool_context *tctx)
{
	tool_registry_cleanup(reg);
	tool_context_destroy(tctx);
}

static void test_write_overwrite_append(int *failures)
{
	struct tool_registry reg;
	struct tool_context *tctx;
	struct tool_result result;
	char *data;

	file_ensure_dir("/tmp/morph_fw_work");
	file_ensure_dir("/tmp/morph_fw_out");
	setup_registry(failures, &reg, &tctx, "/tmp/morph_fw_work", "/tmp/morph_fw_out");
	tool_result_init(&result);

	run_tool(&reg, "{\"op\":\"write\",\"path\":\"a.txt\",\"content\":\"one\"}",
		 &result);
	expect_true(failures, result_ok(&result), "write succeeds");
	data = read_file("/tmp/morph_fw_out/a.txt");
	expect_true(failures, data && strcmp(data, "one") == 0, "write content");
	free(data);

	run_tool(&reg, "{\"op\":\"write\",\"path\":\"a.txt\",\"content\":\"two\"}",
		 &result);
	expect_true(failures, !result_ok(&result) && result_code(&result) == -EEXIST,
		    "write existing fails");

	run_tool(&reg,
		 "{\"op\":\"overwrite\",\"path\":\"a.txt\",\"content\":\"two\"}",
		 &result);
	expect_true(failures, result_ok(&result), "overwrite succeeds");
	run_tool(&reg,
		 "{\"op\":\"append\",\"path\":\"a.txt\",\"content\":\" three\"}",
		 &result);
	expect_true(failures, result_ok(&result), "append succeeds");
	data = read_file("/tmp/morph_fw_out/a.txt");
	expect_true(failures, data && strcmp(data, "two three") == 0, "overwrite append content");
	free(data);

	tool_result_cleanup(&result);
	cleanup_registry(&reg, tctx);
}

static void test_mkdir_copy_rename_delete(int *failures)
{
	struct tool_registry reg;
	struct tool_context *tctx;
	struct tool_result result;
	char *data;

	file_ensure_dir("/tmp/morph_fw_work");
	file_ensure_dir("/tmp/morph_fw_out");
	file_write_all("/tmp/morph_fw_work/source.txt", "copy", 4);
	setup_registry(failures, &reg, &tctx, "/tmp/morph_fw_work", "/tmp/morph_fw_out");
	tool_result_init(&result);

	run_tool(&reg, "{\"op\":\"mkdir\",\"path\":\"dir/sub\"}", &result);
	expect_true(failures, result_ok(&result), "mkdir succeeds");

	run_tool(&reg,
		 "{\"op\":\"copy\",\"path\":\"source.txt\","
		 "\"dst_path\":\"dir/sub/copied.txt\"}",
		 &result);
	expect_true(failures, result_ok(&result), "copy succeeds");
	data = read_file("/tmp/morph_fw_out/dir/sub/copied.txt");
	expect_true(failures, data && strcmp(data, "copy") == 0, "copy content");
	free(data);

	run_tool(&reg,
		 "{\"op\":\"rename\",\"path\":\"dir/sub/copied.txt\","
		 "\"dst_path\":\"dir/sub/moved.txt\"}",
		 &result);
	expect_true(failures, result_ok(&result), "rename succeeds");
	expect_true(failures, access("/tmp/morph_fw_out/dir/sub/copied.txt", F_OK) != 0,
		    "rename removed source");
	expect_true(failures, access("/tmp/morph_fw_out/dir/sub/moved.txt", F_OK) == 0,
		    "rename created dest");

	run_tool(&reg, "{\"op\":\"delete\",\"path\":\"dir/sub/moved.txt\"}",
		 &result);
	expect_true(failures, result_ok(&result), "delete file succeeds");
	run_tool(&reg, "{\"op\":\"delete\",\"path\":\"dir/sub\"}", &result);
	expect_true(failures, result_ok(&result), "delete empty dir succeeds");

	tool_result_cleanup(&result);
	cleanup_registry(&reg, tctx);
}

static void test_base64_and_authorization(int *failures)
{
	struct tool_registry reg;
	struct tool_context *tctx;
	struct tool_result result;
	char *data;

	file_ensure_dir("/tmp/morph_fw_work");
	file_ensure_dir("/tmp/morph_fw_out");
	setup_registry(failures, &reg, &tctx, "/tmp/morph_fw_work", "/tmp/morph_fw_out");
	tool_result_init(&result);

	run_tool(&reg,
		 "{\"op\":\"overwrite\",\"path\":\"b.bin\",\"content\":\"AAEC\","
		 "\"encoding\":\"base64\"}",
		 &result);
	expect_true(failures, result_ok(&result), "base64 write succeeds");
	data = read_file("/tmp/morph_fw_out/b.bin");
	expect_true(failures,
		    data && (unsigned char)data[0] == 0 && (unsigned char)data[1] == 1 &&
			    (unsigned char)data[2] == 2,
		    "base64 bytes");
	free(data);

	run_tool(&reg,
		 "{\"op\":\"overwrite\",\"path\":\"/tmp/morph_fw_denied.txt\","
		 "\"content\":\"no\"}",
		 &result);
	expect_true(failures, !result_ok(&result), "outside output denied");

	tool_result_cleanup(&result);
	cleanup_registry(&reg, tctx);
}

static void test_patch(int *failures)
{
	struct tool_registry reg;
	struct tool_context *tctx;
	struct tool_result result;
	char *data;

	file_ensure_dir("/tmp/morph_fw_work");
	file_ensure_dir("/tmp/morph_fw_out");
	setup_registry(failures, &reg, &tctx, "/tmp/morph_fw_work", "/tmp/morph_fw_out");
	tool_result_init(&result);

	run_tool(&reg,
		 "{\"op\":\"overwrite\",\"path\":\"patch.txt\","
		 "\"content\":\"hello brave world\"}",
		 &result);
	expect_true(failures, result_ok(&result), "patch setup succeeds");

	run_tool(&reg,
		 "{\"op\":\"patch\",\"path\":\"patch.txt\",\"offset\":6,"
		 "\"length\":5,\"content\":\"small\"}",
		 &result);
	expect_true(failures, result_ok(&result), "patch replace succeeds");
	data = read_file("/tmp/morph_fw_out/patch.txt");
	expect_true(failures, data && strcmp(data, "hello small world") == 0,
		    "patch replace content");
	free(data);

	run_tool(&reg,
		 "{\"op\":\"patch\",\"path\":\"patch.txt\",\"offset\":11,"
		 "\"length\":0,\"content\":\" blue\"}",
		 &result);
	expect_true(failures, result_ok(&result), "patch insert succeeds");
	data = read_file("/tmp/morph_fw_out/patch.txt");
	expect_true(failures, data && strcmp(data, "hello small blue world") == 0,
		    "patch insert content");
	free(data);

	run_tool(&reg,
		 "{\"op\":\"patch\",\"path\":\"patch.txt\",\"offset\":100,"
		 "\"length\":1,\"content\":\"bad\"}",
		 &result);
	expect_true(failures, !result_ok(&result) && result_code(&result) == -ERANGE,
		    "patch out of range fails");

	tool_result_cleanup(&result);
	cleanup_registry(&reg, tctx);
}

int main(void)
{
	int failures = 0;

	(void)system("rm -rf /tmp/morph_fw_work /tmp/morph_fw_out "
		     "/tmp/morph_fw_denied.txt");
	test_write_overwrite_append(&failures);
	test_mkdir_copy_rename_delete(&failures);
	test_base64_and_authorization(&failures);
	test_patch(&failures);
	(void)system("rm -rf /tmp/morph_fw_work /tmp/morph_fw_out "
		     "/tmp/morph_fw_denied.txt");
	if (failures) {
		fprintf(stderr, "%d failures\n", failures);
		return 1;
	}
	printf("file_write tests passed\n");
	return 0;
}
