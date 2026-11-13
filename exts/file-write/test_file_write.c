#include "file_write.h"
#include "nb/cleanup.h"
#include "nb/err.h"

#include "agent/tool_context.h"
#include "cJSON.h"
#include "nb/file.h"

#include <errno.h>
#include <fcntl.h>
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

static void test_fd_cleanup(int *fd)
{
	if (*fd >= 0)
		(void)close(*fd);
	*fd = -1;
}

static char *read_file(const char *path)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_1 NB_CLEANUP(nb_charp_cleanup) = NULL;

	size_t len;

	return (nb_file_read_all(path, &file_value_1, 0, NULL) == NB_OK &&
				(file_copy_1 = (char *)malloc(nb_buf_len(&file_value_1) + 1)) !=
					NULL
			? (memcpy(file_copy_1, nb_buf_cstr(&file_value_1),
				  nb_buf_len(&file_value_1) + 1),
			   *(&len) = nb_buf_len(&file_value_1), nb_charp_steal(&file_copy_1))
			: NULL);
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

	root = cJSON_Parse(result->text.ptr ? result->text.ptr : "");
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

	root = cJSON_Parse(result->text.ptr ? result->text.ptr : "");
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

	nb_file_mkdir("/tmp/morph_fw_work", true, NULL);
	nb_file_mkdir("/tmp/morph_fw_out", true, NULL);
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
	expect_true(failures, !result_ok(&result) && result_code(&result) == NB_ERR_EXISTS,
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

	nb_file_mkdir("/tmp/morph_fw_work", true, NULL);
	nb_file_mkdir("/tmp/morph_fw_out", true, NULL);
	nb_file_write_all("/tmp/morph_fw_work/source.txt", "copy", 4, NULL);
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

	nb_file_mkdir("/tmp/morph_fw_work", true, NULL);
	nb_file_mkdir("/tmp/morph_fw_out", true, NULL);
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

	const char *accepted[] = {"AAE=", "AAE", " A A E = \\n\\t"};
	for (size_t i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
		char request[256];
		snprintf(request, sizeof(request),
			 "{\"op\":\"overwrite\",\"path\":\"b.bin\",\"content\":\"%s\","
			 "\"encoding\":\"base64\"}",
			 accepted[i]);
		run_tool(&reg, request, &result);
		expect_true(failures, result_ok(&result), "base64 padding and whitespace");
		data = read_file("/tmp/morph_fw_out/b.bin");
		expect_true(failures, data && data[0] == 0 && data[1] == 1, "base64 binary length");
		free(data);
	}
	const char *rejected[] = {"A", "A===", "AA=E", "AAE$", "AAE==", "AB=="};
	for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
		char request[256];
		snprintf(request, sizeof(request),
			 "{\"op\":\"overwrite\",\"path\":\"b.bin\",\"content\":\"%s\","
			 "\"encoding\":\"base64\"}",
			 rejected[i]);
		run_tool(&reg, request, &result);
		expect_true(failures, !result_ok(&result) && result_code(&result) == NB_ERR_INVAL,
			    "malformed base64 rejected");
		data = read_file("/tmp/morph_fw_out/b.bin");
		expect_true(failures, data && data[0] == 0 && data[1] == 1,
			    "invalid base64 preserves destination");
		free(data);
	}

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

	nb_file_mkdir("/tmp/morph_fw_work", true, NULL);
	nb_file_mkdir("/tmp/morph_fw_out", true, NULL);
	setup_registry(failures, &reg, &tctx, "/tmp/morph_fw_work", "/tmp/morph_fw_out");
	tool_result_init(&result);

	run_tool(&reg,
		 "{\"op\":\"overwrite\",\"path\":\"patch.txt\","
		 "\"content\":\"hello brave world\"}",
		 &result);
	expect_true(failures, result_ok(&result), "patch setup succeeds");
	expect_true(failures, nb_file_set_mode("/tmp/morph_fw_out/patch.txt", 0750, NULL) == NB_OK,
		    "patch executable mode setup");

	run_tool(&reg,
		 "{\"op\":\"patch\",\"path\":\"patch.txt\",\"offset\":6,"
		 "\"length\":5,\"content\":\"small\"}",
		 &result);
	expect_true(failures, result_ok(&result), "patch replace succeeds");
	nb_file_info info;
	expect_true(failures,
		    nb_file_stat("/tmp/morph_fw_out/patch.txt", false, &info, NULL) == NB_OK &&
			    (info.mode & 0777u) == 0750u,
		    "patch preserves executable mode");
	data = read_file("/tmp/morph_fw_out/patch.txt");
	expect_true(failures, data && strcmp(data, "hello small world") == 0,
		    "patch replace content");
	free(data);

	expect_true(failures, nb_file_set_mode("/tmp/morph_fw_out/patch.txt", 0640, NULL) == NB_OK,
		    "patch group-readable mode setup");
	run_tool(&reg,
		 "{\"op\":\"patch\",\"path\":\"patch.txt\",\"offset\":11,"
		 "\"length\":0,\"content\":\" blue\"}",
		 &result);
	expect_true(failures, result_ok(&result), "patch insert succeeds");
	expect_true(failures,
		    nb_file_stat("/tmp/morph_fw_out/patch.txt", false, &info, NULL) == NB_OK &&
			    (info.mode & 0777u) == 0640u,
		    "patch preserves group-readable mode");
	data = read_file("/tmp/morph_fw_out/patch.txt");
	expect_true(failures, data && strcmp(data, "hello small blue world") == 0,
		    "patch insert content");
	free(data);

	run_tool(&reg,
		 "{\"op\":\"patch\",\"path\":\"patch.txt\",\"offset\":100,"
		 "\"length\":1,\"content\":\"bad\"}",
		 &result);
	expect_true(failures, !result_ok(&result) && result_code(&result) == NB_ERR_RANGE,
		    "patch out of range fails");

	const char *large_path = "/tmp/morph_fw_out/large-patch.bin";
	const off_t large_size = 12 * 1024 * 1024;
	expect_true(failures,
		    nb_file_write_all(large_path, "", 0, NULL) == NB_OK &&
			    truncate(large_path, large_size) == 0,
		    "large sparse patch setup");
	run_tool(&reg,
		 "{\"op\":\"patch\",\"path\":\"large-patch.bin\",\"offset\":6291456,"
		 "\"length\":4,\"content\":\"XYZ\"}",
		 &result);
	expect_true(failures, result_ok(&result), "patch larger than content limit succeeds");
	uint64_t size_after = 0;
	expect_true(failures,
		    nb_file_size(large_path, &size_after, NULL) == NB_OK &&
			    size_after == (uint64_t)large_size - 1u,
		    "large patch size");
	int fd NB_CLEANUP(test_fd_cleanup) = open(large_path, O_RDONLY);
	char inserted[3] = {0};
	char last = 1;
	expect_true(failures,
		    fd >= 0 && pread(fd, inserted, sizeof(inserted), 6291456) == 3 &&
			    memcmp(inserted, "XYZ", sizeof(inserted)) == 0 &&
			    pread(fd, &last, 1, large_size - 2) == 1 && last == 0,
		    "large patch preserves binary tail");

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
