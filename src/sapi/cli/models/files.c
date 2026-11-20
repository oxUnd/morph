#include "sapi/cli/models/files.h"

#include "sapi/cli/models/transcript.h"
#include "cJSON.h"
#include <cmark-gfm.h>
#include <nb/cleanup.h>
#include <nb/encoding.h>
#include <nb/err.h>
#include <nb/file.h>
#include <nb/str.h>
#include <nb/utf8.h>
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define CLI_FILES_LIMIT 2048u

void cli_files_init(struct cli_files_model *model)
{
	memset(model, 0, sizeof(*model));
	nb_vec_init(&model->entries, sizeof(struct cli_files_entry));
}

void cli_files_cleanup(struct cli_files_model *model)
{
	for (size_t i = 0; i < model->entries.len; i++)
		free(((struct cli_files_entry *)nb_vec_at(&model->entries, i))->path);

	nb_vec_cleanup(&model->entries);
}

static int cli_files_line(const char *text)
{
	if (!text || !isdigit((unsigned char)*text))
		return 0;

	char *end = NULL;
	long number = strtol(text, &end, 10);
	return end != text && number > 0 && number <= INT_MAX ? (int)number : 0;
}

int cli_files_resolve(const char *directory, const char *url, nb_buf *path, int *line)
{
	if (!directory || !url || !path || path->len)
		return NB_ERR_INVAL;

	if (line)
		*line = 0;

	if (!*url || *url == '#')
		return NB_ERR_UNSUPPORTED;

	if (!strncmp(url, "file://", 7)) {
		url += 7;
		if (!strncmp(url, "localhost/", 10))
			url += 9;

		if (*url != '/')
			return NB_ERR_UNSUPPORTED;
	} else {
		const char *colon = strchr(url, ':');
		const char *slash = strchr(url, '/');
		if (colon && (!slash || colon < slash) &&
		    !isdigit((unsigned char)colon[1]))
			return NB_ERR_UNSUPPORTED;
	}

	char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(url);
	if (!copy)
		return NB_ERR_NOMEM;

	char *fragment = strpbrk(copy, "?#");
	if (fragment) {
		if (*fragment == '#' && line)
			*line = cli_files_line(fragment + (fragment[1] == 'L' ? 2 : 1));

		*fragment = '\0';
	}

	char *colon = strrchr(copy, ':');
	if (colon && cli_files_line(colon + 1)) {
		if (line)
			*line = cli_files_line(colon + 1);

		*colon = '\0';
		colon = strrchr(copy, ':');
		if (colon && cli_files_line(colon + 1)) {
			if (line)
				*line = cli_files_line(colon + 1);

			*colon = '\0';
		}
	}

	nb_buf decoded NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = nb_url_decode(nb_str_from_cstr(copy), false, &decoded, NULL);
	if (rc)
		return rc;

	if (!decoded.len || decoded.len >= PATH_MAX ||
	    nb_utf8_validate(nb_buf_str(&decoded), NULL) != NB_UTF8_OK)
		return NB_ERR_FORMAT;

	for (size_t i = 0; i < decoded.len; i++) {
		unsigned char byte = (unsigned char)decoded.ptr[i];
		if (byte < 32 || byte == 127)
			return NB_ERR_FORMAT;
	}

	nb_buf full NB_CLEANUP(nb_buf_cleanup) = {0};
	if (decoded.ptr[0] == '~') {
		rc = nb_path_expand(&full, nb_buf_cstr(&decoded), NULL);
		return rc ? rc : nb_path_clean(path, nb_buf_cstr(&full));
	}

	rc = nb_path_is_absolute(nb_buf_cstr(&decoded))
		? nb_buf_append_cstr(&full, nb_buf_cstr(&decoded)) ? NB_OK : nb_buf_status(&full)
		: nb_path_join(&full, directory, nb_buf_cstr(&decoded));
	return rc ? rc : nb_path_clean(path, nb_buf_cstr(&full));
}

int cli_files_add(struct cli_files_model *model, const char *directory, const char *url,
	enum cli_files_kind kind)
{
	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
	int line = 0;
	int rc = cli_files_resolve(directory, url, &path, &line);
	if (rc == NB_ERR_UNSUPPORTED || rc == NB_ERR_FORMAT)
		return NB_OK;

	if (rc)
		return rc;

	for (size_t i = 0; i < model->entries.len; i++) {
		struct cli_files_entry *entry = nb_vec_at(&model->entries, i);
		if (!strcmp(entry->path, nb_buf_cstr(&path))) {
			if (kind > entry->kind) {
				entry->kind = kind;
				model->revision++;
			}

			return NB_OK;
		}
	}

	if (model->entries.len >= CLI_FILES_LIMIT) {
		model->truncated = 1;
		return NB_OK;
	}

	char *copy NB_CLEANUP(nb_charp_cleanup) = strdup(nb_buf_cstr(&path));
	if (!copy)
		return NB_ERR_NOMEM;

	struct cli_files_entry *entry = nb_vec_push(&model->entries);
	if (!entry)
		return NB_ERR_NOMEM;

	*entry = (struct cli_files_entry){
		.path = nb_charp_steal(&copy), .kind = kind, .line = line};
	model->revision++;
	return NB_OK;
}

static void cli_files_markdown_cleanup(cmark_node **node)
{
	cmark_node_free(*node);
	*node = NULL;
}

static void cli_files_iterator_cleanup(cmark_iter **iterator)
{
	cmark_iter_free(*iterator);
	*iterator = NULL;
}

int cli_files_markdown(struct cli_files_model *model, const char *directory, const char *text)
{
	if (!text || !*text)
		return NB_OK;

	cmark_node *document NB_CLEANUP(cli_files_markdown_cleanup) =
		cmark_parse_document(text, strlen(text), CMARK_OPT_DEFAULT);
	cmark_iter *iterator NB_CLEANUP(cli_files_iterator_cleanup) =
		document ? cmark_iter_new(document) : NULL;
	if (!iterator)
		return NB_ERR_NOMEM;

	cmark_event_type event;
	while ((event = cmark_iter_next(iterator)) != CMARK_EVENT_DONE) {
		cmark_node *node = cmark_iter_get_node(iterator);
		cmark_node_type type = cmark_node_get_type(node);
		if (event != CMARK_EVENT_ENTER ||
		    (type != CMARK_NODE_LINK && type != CMARK_NODE_IMAGE))
			continue;

		int rc = cli_files_add(model, directory,
			cmark_node_get_url(node), CLI_FILES_REFERENCED);
		if (rc)
			return rc;
	}

	return NB_OK;
}

int cli_files_tool(struct cli_files_model *model, const char *directory,
	const struct cli_transcript_tool *tool)
{
	if (!tool || !tool->result || cli_transcript_tool_is_running(tool) || tool->state < 0)
		return NB_OK;

	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Parse(tool->result);
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(result, "data");
	if (!cJSON_IsObject(data))
		data = result;

	if (!strcmp(tool->name, "apply_patch")) {
		const cJSON *files = cJSON_GetObjectItemCaseSensitive(data, "files");
		const char *root = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(data, "workspace_root"));
		const cJSON *file;
		cJSON_ArrayForEach(file, files) {
			const char *path = cJSON_GetStringValue(
				cJSON_GetObjectItemCaseSensitive(file, "path"));
			const char *action = cJSON_GetStringValue(
				cJSON_GetObjectItemCaseSensitive(file, "action"));
			if (!path || (action && !strcmp(action, "delete")))
				continue;

			int rc = cli_files_add(model, root ? root : directory,
				path, CLI_FILES_MODIFIED);
			if (rc)
				return rc;
		}
	}

	const cJSON *artifacts = cJSON_GetObjectItemCaseSensitive(result, "artifacts");
	const cJSON *artifact;
	cJSON_ArrayForEach(artifact, artifacts) {
		const char *path = cJSON_GetStringValue(
			cJSON_GetObjectItemCaseSensitive(artifact, "path"));
		if (!path)
			continue;

		int rc = cli_files_add(model, directory, path, CLI_FILES_GENERATED);
		if (rc)
			return rc;
	}

	return NB_OK;
}
