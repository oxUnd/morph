#include "content.h"

#include <nb/buf.h>
#include <nb/cleanup.h>
#include <string.h>

static int content_text(cJSON *parts, const char *text, int responses)
{
	cJSON *part NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!part || !cJSON_AddStringToObject(part, "type", responses ? "input_text" : "text") ||
	    !cJSON_AddStringToObject(part, "text", text) || !cJSON_AddItemToArray(parts, part))
		return NB_ERR_NOMEM;

	part = NULL;
	return 0;
}

cJSON *llm_content_create(const char *prefix, const cJSON *parts, int responses)
{
	cJSON *result NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateArray();
	if (!result)
		return NULL;

	if (prefix && prefix[0] && content_text(result, prefix, responses) != 0)
		return NULL;

	const cJSON *input;
	cJSON_ArrayForEach (input, parts) {
		const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(input, "type"));
		if (!type)
			return NULL;

		if (!strcmp(type, "text")) {
			const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(input, "text"));
			if (!text || content_text(result, text, responses) != 0)
				return NULL;

			continue;
		}

		const char *data = cJSON_GetStringValue(cJSON_GetObjectItem(input, "data"));
		const char *mime = cJSON_GetStringValue(cJSON_GetObjectItem(input, "mimeType"));
		if (!data || !mime)
			return NULL;

		cJSON *part NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (!part)
			return NULL;

		if (!strcmp(type, "image")) {
			nb_buf uri NB_CLEANUP(nb_buf_cleanup) = {0};
			nb_buf_init(&uri);
			if (!nb_buf_printf(&uri, "data:%s;base64,%s", mime, data) ||
			    !cJSON_AddStringToObject(part, "type",
						     responses ? "input_image" : "image_url"))
				return NULL;

			if (responses) {
				if (!cJSON_AddStringToObject(part, "image_url", nb_buf_cstr(&uri)))
					return NULL;
			} else {
				cJSON *url = cJSON_AddObjectToObject(part, "image_url");
				if (!url || !cJSON_AddStringToObject(url, "url", nb_buf_cstr(&uri)))
					return NULL;
			}
		} else if (!strcmp(type, "audio")) {
			const char *format =
				!strcmp(mime, "audio/wav") || !strcmp(mime, "audio/x-wav") ? "wav"
				: !strcmp(mime, "audio/mpeg") || !strcmp(mime, "audio/mp3")
					? "mp3"
					: strchr(mime, '/');
			if (!format || !cJSON_AddStringToObject(part, "type", "input_audio"))
				return NULL;

			if (format[0] == '/')
				format++;

			cJSON *audio = cJSON_AddObjectToObject(part, "input_audio");
			if (!audio || !cJSON_AddStringToObject(audio, "data", data) ||
			    !cJSON_AddStringToObject(audio, "format", format))
				return NULL;
		} else {
			return NULL;
		}

		if (!cJSON_AddItemToArray(result, part))
			return NULL;

		part = NULL;
	}

	cJSON *owned = result;
	result = NULL;
	return owned;
}

int llm_content_tool_split(cJSON *parts, cJSON *attachments, int responses)
{
	for (cJSON *part = parts ? parts->child : NULL; part;) {
		cJSON *next = part->next;
		const char *type =
			cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(part, "type"));
		int move =
			type && (responses ? !strcmp(type, "input_audio") : strcmp(type, "text"));
		if (move) {
			cJSON *moved NB_CLEANUP(cJSON_DeleteOwner) =
				cJSON_DetachItemViaPointer(parts, part);
			if (!moved || !cJSON_AddItemToArray(attachments, moved))
				return NB_ERR_NOMEM;

			moved = NULL;
		}

		part = next;
	}

	return 0;
}

int llm_content_attachments_append(cJSON *messages, cJSON *attachments)
{
	if (!attachments || !attachments->child)
		return 0;

	cJSON *message NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	cJSON *parts NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Duplicate(attachments, 1);
	if (!message || !parts || !cJSON_AddStringToObject(message, "role", "user") ||
	    !cJSON_AddItemToObject(message, "content", parts))
		return NB_ERR_NOMEM;

	parts = NULL;
	if (!cJSON_AddItemToArray(messages, message))
		return NB_ERR_NOMEM;

	message = NULL;
	while (attachments->child)
		cJSON_Delete(cJSON_DetachItemFromArray(attachments, 0));

	return 0;
}
