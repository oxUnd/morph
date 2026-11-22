#include "mcp_internal.h"

#include "agent/tool_context.h"
#include "agent/tool_runtime.h"
#include <nb/cleanup.h>
#include <nb/encoding.h>
#include <nb/file.h>
#include <nb/str.h>
#include <stdio.h>
#include <string.h>

struct mcp_content_files {
	nb_buf directory;
	struct tool_artifact_list artifacts;
	int committed;
};

static void mcp_content_files_cleanup(struct mcp_content_files *files)
{
	if (!files->committed) {
		for (int i = 0; i < files->artifacts.count; i++)
			(void)nb_file_remove(files->artifacts.items[i].path, NULL);

		if (files->directory.len)
			(void)nb_file_rmdir(nb_buf_cstr(&files->directory), NULL);
	}

	nb_buf_cleanup(&files->directory);
}

static const char *content_string(const cJSON *value, const char *key)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(value, key));
	return text ? text : "";
}

static const char *content_extension(const char *mime)
{
	const char *types[] = {"image/png",	"image/jpeg", "image/webp", "image/gif",
			       "image/svg+xml", "audio/wav",  "audio/mpeg", "application/pdf"};
	const char *extensions[] = {"png", "jpg", "webp", "gif", "svg", "wav", "mp3", "pdf"};
	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		if (!strcmp(mime, types[i]))
			return extensions[i];
	}

	return "bin";
}

static int content_save(cJSON *block, const char *key, const char *output_dir,
			struct mcp_content_files *files)
{
	const char *data = content_string(block, key);
	const char *mime = content_string(block, "mimeType");
	if (!data[0] && !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(block, key)))
		return NB_ERR_PROTOCOL;

	if (files->artifacts.count >= TOOL_ARTIFACT_MAX)
		return NB_ERR_NO_SPACE;

	nb_buf bytes NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = nb_base64_decode(nb_str_from_cstr(data), NB_B64_STD, &bytes, NULL);
	if (rc != 0)
		return NB_ERR_PROTOCOL;

	if (!files->directory.len) {
		if (output_dir && output_dir[0]) {
			rc = nb_file_mkdir(output_dir, true, NULL);
			if (rc != 0 && rc != NB_ERR_EXISTS)
				return rc;
		}

		rc = nb_file_temp_dir(&files->directory, output_dir, "mcp-", NULL);
		if (rc != 0)
			return rc;
	}

	struct tool_artifact *artifact = &files->artifacts.items[files->artifacts.count];
	int size = snprintf(artifact->path, sizeof(artifact->path), "%s/content-%d.%s",
			    nb_buf_cstr(&files->directory), files->artifacts.count,
			    content_extension(mime));
	if (size < 0 || (size_t)size >= sizeof(artifact->path))
		return NB_ERR_NAME_TOO_LONG;

	files->artifacts.count++;
	const nb_file_write_options options = {.mode = 0600, .create_only = true};
	rc = nb_file_write_all_atomic(artifact->path, bytes.ptr, bytes.len, &options, NULL);
	if (rc != 0)
		return rc;

	artifact->kind = !strncmp(mime, "image/", 6) ? TOOL_ARTIFACT_IMAGE : TOOL_ARTIFACT_FILE;
	(void)snprintf(artifact->mime, sizeof(artifact->mime), "%s", mime);
	(void)snprintf(artifact->label, sizeof(artifact->label), "%s", "MCP attachment");
	artifact->size_bytes = (long long)bytes.len;
	if (!cJSON_AddStringToObject(block, "path", artifact->path) ||
	    !cJSON_AddNumberToObject(block, "sizeBytes", (double)bytes.len))
		return NB_ERR_NOMEM;

	cJSON_DeleteItemFromObjectCaseSensitive(block, key);
	return 0;
}

static int content_block(cJSON *block, const char *output_dir, struct mcp_content_files *files)
{
	const char *type = content_string(block, "type");
	if (!strcmp(type, "image") || !strcmp(type, "audio"))
		return content_save(block, "data", output_dir, files);

	if (cJSON_HasObjectItem(block, "blob"))
		return content_save(block, "blob", output_dir, files);

	if (!strcmp(type, "resource"))
		return content_block(cJSON_GetObjectItemCaseSensitive(block, "resource"),
				     output_dir, files);

	return 0;
}

static int content_array(cJSON *array, const char *output_dir, struct mcp_content_files *files)
{
	if (!array)
		return 0;

	if (cJSON_IsObject(array))
		return content_block(array, output_dir, files);

	if (!cJSON_IsArray(array))
		return 0;

	for (cJSON *block = array->child; block; block = block->next) {
		int rc = content_block(block, output_dir, files);
		if (rc != 0)
			return rc;
	}

	return 0;
}

int mcp_result_content_copy(struct tool_result *out, const cJSON *value)
{
	if (!out || !cJSON_IsObject(value))
		return NB_ERR_INVAL;

	cJSON *copy NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Duplicate(value, 1);
	struct mcp_content_files files NB_CLEANUP(mcp_content_files_cleanup) = {0};
	if (!copy)
		return NB_ERR_NOMEM;

	const char *output_dir =
		out->runtime && out->runtime->permission_context
			? tool_context_output_dir_borrow(out->runtime->permission_context)
			: NULL;
	const char *keys[] = {"content", "contentBlocks", "contents"};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		int rc = content_array(cJSON_GetObjectItemCaseSensitive(copy, keys[i]), output_dir,
				       &files);
		if (rc != 0)
			return rc;
	}

	cJSON *messages = cJSON_GetObjectItemCaseSensitive(copy, "messages");
	for (cJSON *message = messages ? messages->child : NULL; message; message = message->next) {
		int rc = content_array(cJSON_GetObjectItemCaseSensitive(message, "content"),
				       output_dir, &files);
		if (rc != 0)
			return rc;
	}

	int rc = tool_result_success_copy(out, copy);
	if (rc == 0 && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(copy, "isError"))) {
		cJSON *error = cJSON_AddObjectToObject(out->envelope, "error");
		cJSON *ok NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateFalse();
		const char *message = content_string(copy, "error");
		if (!message[0])
			message = "MCP tool returned isError";

		if (!error || !ok || !cJSON_AddStringToObject(error, "code", "mcp_tool_error") ||
		    !cJSON_AddStringToObject(error, "message", message) ||
		    !cJSON_ReplaceItemInObjectCaseSensitive(out->envelope, "ok", ok))
			return NB_ERR_NOMEM;

		ok = NULL;
	}

	if (rc == 0) {
		out->artifacts = files.artifacts;
		files.committed = 1;
	}

	return rc;
}
