/* artifacts.c — zero-copy output_dir artifact access */
#define _GNU_SOURCE
#include "handlers.h"
#include "../session_store.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"

__attribute__((weak)) const char *fcgi_artifact_output_dir(void);

static const char *artifact_root(void)
{
	if (fcgi_artifact_output_dir)
		return fcgi_artifact_output_dir();
	return "/var/lib/morph/output";
}

static int path_within_root(const char *root, const char *rel,
			    char out_path[PATH_MAX])
{
	char joined[PATH_MAX];
	char root_real[PATH_MAX];
	char file_real[PATH_MAX];
	size_t root_len;

	if (!root || !rel || rel[0] == '/' || strstr(rel, ".."))
		return 0;
	if (snprintf(joined, sizeof(joined), "%s/%s", root, rel) >=
	    (int)sizeof(joined))
		return 0;
	if (!realpath(root, root_real))
		return 0;
	if (!realpath(joined, file_real))
		return 0;
	root_len = strlen(root_real);
	if (strncmp(root_real, file_real, root_len) != 0)
		return 0;
	if (file_real[root_len] != '/' && file_real[root_len] != '\0')
		return 0;
	snprintf(out_path, PATH_MAX, "%s", file_real);
	return 1;
}

static char *artifact_meta_json(const struct artifact_record *a)
{
	cJSON *root = cJSON_CreateObject();
	char url[128];
	char *json;

	if (!root)
		return NULL;
	snprintf(url, sizeof(url), "/api/artifacts/%s", a->id);
	cJSON_AddStringToObject(root, "id", a->id);
	cJSON_AddStringToObject(root, "session_id", a->session_id);
	cJSON_AddStringToObject(root, "kind", a->kind);
	cJSON_AddStringToObject(root, "mime", a->mime);
	cJSON_AddStringToObject(root, "filename", a->filename);
	cJSON_AddNumberToObject(root, "size_bytes", (double)a->size_bytes);
	cJSON_AddStringToObject(root, "status", a->status);
	cJSON_AddNumberToObject(root, "created_at", (double)a->created_at);
	cJSON_AddStringToObject(root, "url", url);
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return json;
}

void handle_list_artifacts(request_t *r)
{
	const char *sid = path_param(r, "id");
	char *json = NULL;

	if (!sid) {
		reply_400(r, "missing id");
		return;
	}
	if (!store_session_owned_by(r->store, sid, r->user_id)) {
		reply_403(r);
		return;
	}
	if (store_artifact_list_json(r->store, r->user_id, sid, &json) != 0) {
		reply_500(r, "artifact list failed");
		return;
	}
	reply_200_json(r, json);
	free(json);
}

void handle_artifact_meta(request_t *r)
{
	const char *aid = path_param(r, "artifact");
	struct artifact_record a;
	char *json;

	if (!aid) {
		reply_400(r, "missing artifact");
		return;
	}
	if (store_artifact_get(r->store, aid, r->user_id, &a) != 0) {
		reply_404(r);
		return;
	}
	json = artifact_meta_json(&a);
	if (!json) {
		reply_500(r, "oom");
		return;
	}
	reply_200_json(r, json);
	free(json);
}

void handle_get_artifact(request_t *r)
{
	const char *aid = path_param(r, "artifact");
	struct artifact_record a;
	char path[PATH_MAX];
	struct stat st;
	FILE *fp;
	char buf[BUFSIZ];
	size_t rd;

	if (!aid) {
		reply_400(r, "missing artifact");
		return;
	}
	if (store_artifact_get(r->store, aid, r->user_id, &a) != 0) {
		reply_404(r);
		return;
	}
	if (!path_within_root(artifact_root(), a.relative_path, path)) {
		reply_404(r);
		return;
	}
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		reply_404(r);
		return;
	}
	fp = fopen(path, "rb");
	if (!fp) {
		reply_500(r, "open artifact failed");
		return;
	}
	FCGX_FPrintF(r->fcgx->out,
		"Status: 200 OK\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %lld\r\n"
		"Cache-Control: private, no-store\r\n"
		"Content-Disposition: attachment; filename=\"%s\"\r\n"
		"\r\n",
		a.mime[0] ? a.mime : "application/octet-stream",
		(long long)st.st_size,
		a.filename[0] ? a.filename : a.id);
	while ((rd = fread(buf, 1, sizeof(buf), fp)) > 0)
		FCGX_PutStr(buf, (int)rd, r->fcgx->out);
	fclose(fp);
}
