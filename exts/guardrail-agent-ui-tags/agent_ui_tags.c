#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tag_allowed(const char *name)
{
	if (!name) return 0;
	static const char *allowed[] = {
		"speak", "vocab", "sentence", "button",
		"navigate", "highlight", "copy", NULL
	};
	for (const char **tag = allowed; *tag; tag++) {
		if (strcmp(name, *tag) == 0)
			return 1;
	}
	return 0;
}

static int append_tag(char *out, size_t cap, const char *tag)
{
	if (!out || cap == 0 || !tag || !*tag)
		return 0;
	if (strstr(out, tag))
		return 0;
	size_t used = strlen(out);
	size_t need = strlen(tag) + (used ? 2 : 0);
	if (used + need + 1 >= cap)
		return -ENOSPC;
	if (used) {
		strcat(out, ", ");
		used += 2;
	}
	strcat(out + used, tag);
	return 0;
}

static int find_unsupported_tags(const char *text, char *unknown, size_t cap)
{
	const char *p = text;
	while ((p = strstr(p, "<m:")) != NULL) {
		const char *start = p + 3;
		if (!isalpha((unsigned char)*start)) {
			p += 3;
			continue;
		}

		char tag[64];
		size_t len = 0;
		while (start[len] &&
		       (isalnum((unsigned char)start[len]) ||
		        start[len] == '_' || start[len] == '-' ||
		        start[len] == '.')) {
			if (len + 1 < sizeof(tag))
				tag[len] = start[len];
			len++;
		}

		if (len > 0) {
			size_t copy_len = len < sizeof(tag) ? len : sizeof(tag) - 1;
			tag[copy_len] = '\0';
			if (!tag_allowed(tag))
				append_tag(unknown, cap, tag);
		}
		p = start + len;
	}
	return unknown[0] ? 1 : 0;
}

static char *json_result(int passed, const char *reason)
{
	if (!reason)
		reason = "";
	size_t cap = strlen(reason) + 64;
	char *buf = (char *)malloc(cap);
	if (!buf)
		return NULL;
	snprintf(buf, cap, "{\"pass\":%s,\"reason\":\"%s\"}",
		 passed ? "true" : "false", reason);
	return buf;
}

int guardrail_check(const char *text, const char *rule,
		    const char *description, char **result_json)
{
	(void)rule;
	(void)description;
	if (!result_json)
		return -1;
	if (!text || !*text) {
		*result_json = json_result(1, "");
		return *result_json ? 0 : -1;
	}

	char unknown[256] = {0};
	if (!find_unsupported_tags(text, unknown, sizeof(unknown))) {
		*result_json = json_result(1, "");
		return *result_json ? 0 : -1;
	}

	char reason[512];
	snprintf(reason, sizeof(reason),
		 "Unsupported Agent UI tag(s): %s. Only m:speak, m:vocab, "
		 "m:sentence, m:button, m:navigate, m:highlight, and m:copy "
		 "are supported. ask_user is a tool call, not an m:* tag.",
		 unknown);
	*result_json = json_result(0, reason);
	return *result_json ? 0 : -1;
}
