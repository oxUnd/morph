#include "sapi/fastcgi/presentation/media_refs.h"
#include "nb/cleanup.h"
#include "nb/file.h"
#include "sapi/fastcgi/services/turn_job_internal.h"
#include "sapi/fastcgi/agent_bridge.h"
#include "sapi/fastcgi/session_store.h"
#include "sapi/fastcgi/storage/events.h"
#include "sapi/fastcgi/storage/artifacts.h"
#include "cJSON.h"
#include "errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nb/file.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/file.h"
#include <sys/stat.h>

static const char *bridge_output_dir(struct fcgi_turn_job *j)
{
	return fcgi_artifact_output_dir_borrow(j->bridge);
}

static const char *artifact_mime(const char *kind, const char *path)
{
	const char *dot = strrchr(path, '.');

	if (strcmp(kind, "image") == 0) {
		if (dot && strcmp(dot, ".jpg") == 0)
			return "image/jpeg";
		if (dot && strcmp(dot, ".jpeg") == 0)
			return "image/jpeg";
		if (dot && strcmp(dot, ".webp") == 0)
			return "image/webp";
		return "image/png";
	}
	if (dot && strcmp(dot, ".webm") == 0)
		return "video/webm";
	return "video/mp4";
}

static const char *artifact_kind_for_path(const char *path)
{
	const char *dot;

	if (!path)
		return NULL;
	dot = strrchr(path, '.');
	if (!dot)
		return NULL;
	if (strcmp(dot, ".png") == 0 || strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0 ||
	    strcmp(dot, ".webp") == 0)
		return "image";
	if (strcmp(dot, ".mp4") == 0 || strcmp(dot, ".webm") == 0 || strcmp(dot, ".mov") == 0 ||
	    strcmp(dot, ".mkv") == 0)
		return "video";
	return NULL;
}

static int relative_under_root(const char *root, const char *path, char rel[512])
{
	nb_buf relative NB_CLEANUP(nb_buf_destroy) = {0};
	return nb_path_relative_under(&relative, path, root, false, NULL) == NB_OK &&
	       !nb_buf_empty(&relative) && nb_str_copy_to(nb_buf_str(&relative), rel, 512) == NB_OK;
}

static void artifact_publish_ready(struct fcgi_turn_job *j, const struct fcgi_turn_artifact *a)
{
	cJSON *obj;
	char *json;

	obj = cJSON_CreateObject();
	if (!obj)
		return;
	cJSON_AddStringToObject(obj, "id", a->id);
	cJSON_AddStringToObject(obj, "kind", a->kind);
	cJSON_AddStringToObject(obj, "mime", a->mime);
	cJSON_AddStringToObject(obj, "filename", a->filename);
	cJSON_AddNumberToObject(obj, "size_bytes", (double)a->size_bytes);
	cJSON_AddStringToObject(obj, "url", a->url);
	cJSON_AddStringToObject(obj, "turn_id", j->turn_id);
	json = cJSON_PrintUnformatted(obj);
	cJSON_Delete(obj);
	if (!json)
		return;
	events_publish(j->store, j->session_id, "artifact_ready", json);
	free(json);
}

struct fcgi_turn_artifact *fcgi_media_artifact_borrow(struct fcgi_turn_job *j, const char *path,
						      const char *kind)
{
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};

	char rel[512];
	char real[PATH_MAX];
	char *expanded;
	struct stat st;
	const char *filename;
	struct fcgi_turn_artifact *a;
	char artifact_id[64];

	if (!j || !path || !kind)
		return NULL;
	expanded = (nb_path_expand(&file_value_1, path, NULL) == NB_OK
			    ? strdup(nb_buf_cstr(&file_value_1))
			    : NULL);
	if (!expanded)
		return NULL;
	if (stat(expanded, &st) != 0 || !S_ISREG(st.st_mode)) {
		free(expanded);
		return NULL;
	}
	if (!realpath(expanded, real)) {
		free(expanded);
		return NULL;
	}
	free(expanded);
	if (!relative_under_root(bridge_output_dir(j), real, rel))
		return NULL;
	for (int i = 0; i < j->artifacts_count; i++) {
		if (strcmp(j->artifacts[i].path, real) == 0)
			return &j->artifacts[i];
	}
	if (j->artifacts_count >= (int)(sizeof(j->artifacts) / sizeof(j->artifacts[0])))
		return NULL;

	filename = strrchr(real, '/');
	filename = filename ? filename + 1 : real;
	a = &j->artifacts[j->artifacts_count];
	memset(a, 0, sizeof(*a));
	snprintf(a->path, sizeof(a->path), "%s", real);
	snprintf(a->kind, sizeof(a->kind), "%s", kind);
	snprintf(a->mime, sizeof(a->mime), "%s", artifact_mime(kind, real));
	snprintf(a->filename, sizeof(a->filename), "%.*s", (int)sizeof(a->filename) - 1, filename);
	a->size_bytes = st.st_size;
	if (store_artifact_register(j->store, j->user_id, j->session_id, a->kind, a->mime,
				    a->filename, rel, st.st_size, artifact_id) != 0) {
		memset(a, 0, sizeof(*a));
		return NULL;
	}
	snprintf(a->id, sizeof(a->id), "%s", artifact_id);
	snprintf(a->url, sizeof(a->url), "/api/artifacts/%s", artifact_id);
	j->artifacts_count++;
	artifact_publish_ready(j, a);
	return a;
}

static int path_char(int c)
{
	return c > 32 && c != '"' && c != '\'' && c != '<' && c != '>' && c != '[' && c != ']' &&
	       c != '(' && c != ')' && c != '{' && c != '}';
}

static void trim_candidate_path(char *path)
{
	size_t len;

	len = strlen(path);
	while (len > 0 && (path[len - 1] == ')' || path[len - 1] == ',' || path[len - 1] == '.' ||
			   path[len - 1] == ';' || path[len - 1] == ':' || path[len - 1] == '!')) {
		path[len - 1] = '\0';
		len--;
	}
}

static void html_attr_escape(const char *src, char *dst, size_t dst_size)
{
	size_t pos = 0;

	if (!dst || dst_size == 0)
		return;
	dst[0] = '\0';
	if (!src)
		return;
	for (const char *p = src; *p && pos + 1 < dst_size; p++) {
		const char *rep = NULL;
		switch (*p) {
		case '&':
			rep = "&amp;";
			break;
		case '"':
			rep = "&quot;";
			break;
		case '<':
			rep = "&lt;";
			break;
		case '>':
			rep = "&gt;";
			break;
		default:
			break;
		}
		if (rep) {
			size_t rlen = strlen(rep);
			if (pos + rlen >= dst_size)
				break;
			memcpy(dst + pos, rep, rlen);
			pos += rlen;
		} else {
			dst[pos++] = *p;
		}
	}
	dst[pos] = '\0';
}

static int append_artifact_tag(nb_buf *buf, const struct fcgi_turn_artifact *a)
{
	char tag[1400];
	char filename[512];
	char url[256];

	if (!a)
		return NB_ERR_INVAL;
	html_attr_escape(a->filename, filename, sizeof(filename));
	html_attr_escape(a->url, url, sizeof(url));
	if (strcmp(a->kind, "image") == 0) {
		snprintf(tag, sizeof(tag),
			 "<a href=\"%s\">"
			 "<img data-src=\"%s\" alt=\"%s\" loading=\"lazy\">"
			 "</a>",
			 url, url, filename);
	} else {
		snprintf(tag, sizeof(tag),
			 "<a href=\"%s\">"
			 "<video data-src=\"%s\" controls preload=\"metadata\""
			 "></video></a>",
			 url, url);
	}
	return (nb_buf_append_cstr(buf, tag) ? 0 : NB_ERR_NOMEM);
}

static int candidate_path(struct fcgi_turn_job *j, const char *token, char out[PATH_MAX])
{
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};
	int file_status_2 = NB_OK;

	if (!token || !*token)
		return 0;
	if (token[0] == '/' || token[0] == '~') {
		snprintf(out, PATH_MAX, "%s", token);
		return 1;
	}
	if (((file_status_2 = nb_path_concat(&file_value_2, bridge_output_dir(j), token)) != NB_OK
		     ? file_status_2
	     : (file_status_2 = nb_str_copy_to(nb_buf_str(&file_value_2), out, PATH_MAX)) ==
			     NB_ERR_RANGE
		     ? NB_ERR_NAME_TOO_LONG
		     : file_status_2) != 0)
		return 0;
	return 1;
}

char *fcgi_media_refs_render_copy(struct fcgi_turn_job *j, const char *text)
{
	const char *p;
	nb_buf out NB_CLEANUP(nb_buf_cleanup) = {0};

	if (!text)
		return strdup("");
	if ((nb_buf_init(&out), nb_buf_reserve(&out, 256) ? 0 : NB_ERR_NOMEM) < 0)
		return strdup("");

	p = text;
	while (*p) {
		const char *start = p;
		char token[PATH_MAX];
		char path[PATH_MAX];
		const char *kind;
		struct fcgi_turn_artifact *a;
		size_t raw_len;
		size_t tok_len;

		while (*p && !path_char((unsigned char)*p))
			p++;
		if (p > start)
			nb_buf_append(&out, start, (size_t)(p - start));
		start = p;
		while (*p && path_char((unsigned char)*p))
			p++;
		raw_len = (size_t)(p - start);
		if (raw_len == 0)
			continue;
		tok_len = raw_len < sizeof(token) ? raw_len : sizeof(token) - 1;
		snprintf(token, sizeof(token), "%.*s", (int)tok_len, start);
		trim_candidate_path(token);
		kind = artifact_kind_for_path(token);
		if (!kind || !candidate_path(j, token, path)) {
			nb_buf_append(&out, start, raw_len);
			continue;
		}
		a = fcgi_media_artifact_borrow(j, path, kind);
		if (!a) {
			nb_buf_append(&out, start, raw_len);
			continue;
		}
		append_artifact_tag(&out, a);
		if (strlen(token) < raw_len)
			nb_buf_append(&out, start + strlen(token), raw_len - strlen(token));
	}
	if (out.len == 0) {
		nb_buf_cleanup(&out);
		return strdup(text);
	}
	return (nb_buf_ok(&out) ? strdup(nb_buf_cstr(&out)) : NULL);
}

char *fcgi_media_refs_store_copy(const char *text, void *user_data)
{
	return fcgi_media_refs_render_copy((struct fcgi_turn_job *)user_data, text);
}

char *fcgi_media_refs_summary_copy(struct fcgi_turn_job *j)
{
	nb_buf out NB_CLEANUP(nb_buf_cleanup) = {0};

	if (!j || j->artifacts_count <= 0)
		return NULL;
	if ((nb_buf_init(&out), nb_buf_reserve(&out, 256) ? 0 : NB_ERR_NOMEM) < 0)
		return NULL;
	nb_buf_append_cstr(&out, "Generation stopped after reaching the iteration limit, "
				 "but these artifacts were created:\n\n");
	for (int i = 0; i < j->artifacts_count; i++) {
		struct fcgi_turn_artifact *a = &j->artifacts[i];
		nb_buf_printf(&out, "%d. `%s`\n\n", i + 1, a->path);
		append_artifact_tag(&out, a);
		nb_buf_append_cstr(&out, "\n\n");
	}
	return (nb_buf_ok(&out) ? strdup(nb_buf_cstr(&out)) : NULL);
}
