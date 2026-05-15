#include "img_edit.h"
#include "util/log.h"
#include "util/base64.h"
#include "http/client.h"
#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static struct model *g_llm;

static const char *mime_type(const char *path)
{
	const char *ext = strrchr(path, '.');
	if (!ext) return "image/png";
	if (strcasecmp(ext, ".png") == 0) return "image/png";
	if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
	if (strcasecmp(ext, ".gif") == 0) return "image/gif";
	if (strcasecmp(ext, ".webp") == 0) return "image/webp";
	if (strcasecmp(ext, ".bmp") == 0) return "image/bmp";
	return "image/png";
}

static int img_edit_exec(const char *args_json, char **result_json, void *user_data)
{
	(void)user_data;
	if (!result_json) return -EINVAL;

	cJSON *root = cJSON_Parse(args_json);
	if (!root) {
		*result_json = strdup("{\"error\":\"invalid JSON\"}");
		return -EINVAL;
	}
	cJSON *f = cJSON_GetObjectItem(root, "file_path");
	cJSON *p = cJSON_GetObjectItem(root, "prompt");
	const char *file_path = cJSON_IsString(f) ? f->valuestring : NULL;
	const char *prompt = cJSON_IsString(p) ? p->valuestring : NULL;
	if (!file_path || !prompt) {
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"missing file_path or prompt\"}");
		return -EINVAL;
	}

	if (!g_llm || !g_llm->api_key[0]) {
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"no LLM configured\"}");
		return -ENOSYS;
	}

	char *b64 = base64_encode_file(file_path);
	if (!b64) {
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"failed to read image\"}");
		return -EIO;
	}
	const char *mime = mime_type(file_path);

	char data_uri[32768];
	int uri_len = snprintf(data_uri, sizeof(data_uri),
			       "data:%s;base64,%s", mime, b64);
	free(b64);
	if (uri_len < 0 || (size_t)uri_len >= sizeof(data_uri)) {
		cJSON_Delete(root);
		*result_json = strdup("{\"error\":\"image too large\"}");
		return -EFBIG;
	}

	cJSON *msgs = cJSON_CreateArray();
	cJSON *msg = cJSON_CreateObject();
	cJSON *content_arr = cJSON_CreateArray();

	cJSON *text_part = cJSON_CreateObject();
	cJSON_AddStringToObject(text_part, "type", "text");
	cJSON_AddStringToObject(text_part, "text", prompt);
	cJSON_AddItemToArray(content_arr, text_part);

	cJSON *img_part = cJSON_CreateObject();
	cJSON_AddStringToObject(img_part, "type", "image_url");
	cJSON *url_obj = cJSON_CreateObject();
	cJSON_AddStringToObject(url_obj, "url", data_uri);
	cJSON_AddItemToObject(img_part, "image_url", url_obj);
	cJSON_AddItemToArray(content_arr, img_part);

	cJSON_AddStringToObject(msg, "role", "user");
	cJSON_AddItemToObject(msg, "content", content_arr);
	cJSON_AddItemToArray(msgs, msg);

	cJSON *body_obj = cJSON_CreateObject();
	cJSON_AddStringToObject(body_obj, "model", g_llm->model_id);
	cJSON_AddItemToObject(body_obj, "messages", msgs);
	cJSON_AddNumberToObject(body_obj, "max_tokens",
				g_llm->max_tokens > 0 ? g_llm->max_tokens : 4096);
	char *body_str = cJSON_PrintUnformatted(body_obj);
	cJSON_Delete(body_obj);

	char url[512];
	snprintf(url, sizeof(url), "%s/chat/completions",
		 g_llm->api_base[0] ? g_llm->api_base : "https://api.openai.com/v1");

	char auth_hdr[512];
	snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", g_llm->api_key);
	const char *hdrs[] = { auth_hdr };

	struct http_response resp = {0};
	int rc = http_post_ex(url, body_str, strlen(body_str),
			      "application/json", hdrs, 1, &resp);
	free(body_str);

	if (rc < 0 || resp.status_code != 200) {
		log_err("img_edit: Vision API returned HTTP %d", resp.status_code);
		http_response_free(&resp);
		*result_json = strdup("{\"error\":\"Vision API call failed\"}");
		cJSON_Delete(root);
		return -EIO;
	}

	cJSON *resp_root = cJSON_Parse(resp.body);
	http_response_free(&resp);
	const char *answer = "";
	if (resp_root) {
		cJSON *choices = cJSON_GetObjectItem(resp_root, "choices");
		if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
			cJSON *first = cJSON_GetArrayItem(choices, 0);
			cJSON *msg_obj = cJSON_GetObjectItem(first, "message");
			cJSON *cnt = cJSON_GetObjectItem(msg_obj, "content");
			if (cJSON_IsString(cnt)) answer = cnt->valuestring;
		}
	}
	*result_json = strdup(answer ? answer : "(empty)");
	cJSON_Delete(resp_root);
	cJSON_Delete(root);
	return 0;
}

int img_edit_init(struct tool_registry *reg, struct model *llm)
{
	if (!reg) return -EINVAL;
	g_llm = llm;
	return tool_register(reg, "img_edit",
		"Analyze or answer questions about an image. Provide file_path and prompt describing what to look for.",
		"{\"type\":\"object\",\"properties\":{\"file_path\":{\"type\":\"string\"},\"prompt\":{\"type\":\"string\"}}}",
		img_edit_exec, NULL);
}