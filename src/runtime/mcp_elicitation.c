#include "runtime/mcp_elicitation.h"

#include "agent/tool_runtime.h"
#include <nb/buf.h>
#include <nb/cleanup.h>
#include <nb/str.h>
#include <nb/utf8.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct elicitation_answers {
	char **items;
	int count;
};

static void elicitation_answers_cleanup(struct elicitation_answers *answers)
{
	for (int i = 0; i < answers->count; i++)
		free(answers->items[i]);

	free(answers->items);
}

static const char *json_text(const cJSON *object, const char *key)
{
	const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));

	return text ? text : "";
}

static int elicit_ask(const struct tool_runtime_context *context, const char *question,
		      const char *const *choices, int count, const char *mode, int min, int max,
		      struct elicitation_answers *answers)
{
	if (!context || !context->ask_user_fn)
		return NB_ERR_CANCELLED;

	struct ask_user_options options = {.cancel_token = context->http.cancel_token};
	int rc =
		context->ask_user_fn(question, choices, count, mode, min, max, &options,
				     &answers->items, &answers->count, context->ask_user_user_data);
	if (rc == NB_ERR_UNSUPPORTED || rc == NB_ERR_PERMISSION || rc == NB_ERR_NOT_PERMITTED)
		return NB_ERR_CANCELLED;

	if (rc != 0)
		return rc;

	if (answers->count < min || answers->count > max || (answers->count > 0 && !answers->items))
		return NB_ERR_PROTOCOL;

	for (int i = 0; i < answers->count; i++) {
		if (!answers->items[i] ||
		    nb_utf8_validate(nb_str_from_cstr(answers->items[i]), NULL) != NB_UTF8_OK)
			return NB_ERR_PROTOCOL;
	}

	return 0;
}

static int elicit_action(const char *action, cJSON **content, cJSON **result)
{
	cJSON *reply NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!reply || !cJSON_AddStringToObject(reply, "action", action))
		return NB_ERR_NOMEM;

	if (!strcmp(action, "accept") && content && *content) {
		if (!cJSON_AddItemToObject(reply, "content", *content))
			return NB_ERR_NOMEM;

		*content = NULL;
	}

	*result = reply;
	reply = NULL;
	return 0;
}

static const cJSON *enum_option(const cJSON *schema, int index)
{
	const cJSON *values = cJSON_GetObjectItemCaseSensitive(schema, "enum");
	if (cJSON_IsArray(values))
		return cJSON_GetArrayItem(values, index);

	values = cJSON_GetObjectItemCaseSensitive(schema, "oneOf");
	if (!cJSON_IsArray(values))
		values = cJSON_GetObjectItemCaseSensitive(schema, "anyOf");

	return cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(values, index), "const");
}

static int enum_count(const cJSON *schema)
{
	const cJSON *values = cJSON_GetObjectItemCaseSensitive(schema, "enum");
	if (!values)
		values = cJSON_GetObjectItemCaseSensitive(schema, "oneOf");

	if (!values)
		values = cJSON_GetObjectItemCaseSensitive(schema, "anyOf");

	return cJSON_GetArraySize(values);
}

static int value_in_enum(const cJSON *schema, const cJSON *value)
{
	int count = enum_count(schema);
	for (int i = 0; i < count; i++) {
		if (cJSON_Compare(enum_option(schema, i), value, 1))
			return 1;
	}

	return count == 0;
}

static int within_bound(const cJSON *schema, const char *name, double value, int minimum)
{
	const cJSON *bound = cJSON_GetObjectItemCaseSensitive(schema, name);

	return !bound || (cJSON_IsNumber(bound) &&
			  (minimum ? value >= bound->valuedouble : value <= bound->valuedouble));
}

static int uri_valid(const char *text)
{
	const char *colon = strchr(text, ':');
	if (!colon || colon == text ||
	    !((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z')) ||
	    strpbrk(text, "\r\n\t "))
		return 0;

	for (const char *p = text + 1; p < colon; p++) {
		if (!nb_ascii_is_alnum_or((unsigned char)*p, "+.-"))
			return 0;
	}

	return colon[1] != '\0';
}

static int date_valid(const char *text, int timestamp)
{
	if (strlen(text) < 10)
		return 0;

	for (int i = 0; i < 10; i++) {
		if (i == 4 || i == 7) {
			if (text[i] != '-')
				return 0;
		} else if (text[i] < '0' || text[i] > '9') {
			return 0;
		}
	}

	int year = 0;
	int month = 0;
	int day = 0;
	if (sscanf(text, "%4d-%2d-%2d", &year, &month, &day) != 3 || month < 1 || month > 12)
		return 0;

	const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	int leap = month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
	if (day < 1 || day > days[month - 1] + leap)
		return 0;

	if (!timestamp)
		return text[10] == '\0';

	if (strlen(text) < 20 || (text[10] != 'T' && text[10] != 't'))
		return 0;

	for (int i = 11; i < 19; i++) {
		if (i == 13 || i == 16) {
			if (text[i] != ':')
				return 0;
		} else if (text[i] < '0' || text[i] > '9') {
			return 0;
		}
	}

	int hour = 0;
	int minute = 0;
	int second = 0;
	if (sscanf(text + 11, "%2d:%2d:%2d", &hour, &minute, &second) != 3 || hour > 23 ||
	    minute > 59 || second > 60)
		return 0;

	const char *zone = text + 19;
	if (*zone == '.') {
		zone++;
		const char *start = zone;
		while (*zone >= '0' && *zone <= '9')
			zone++;

		if (zone == start)
			return 0;
	}

	if ((*zone == 'Z' || *zone == 'z') && !zone[1])
		return 1;

	if (strlen(zone) != 6 || (*zone != '+' && *zone != '-') || zone[3] != ':')
		return 0;

	for (int i = 1; i < 6; i++) {
		if (i != 3 && (zone[i] < '0' || zone[i] > '9'))
			return 0;
	}

	return (zone[1] - '0') * 10 + zone[2] - '0' <= 23 &&
	       (zone[4] - '0') * 10 + zone[5] - '0' <= 59;
}

static int string_format_valid(const cJSON *schema, const char *value)
{
	const char *format = json_text(schema, "format");
	if (!format[0])
		return 1;

	if (!strcmp(format, "uri"))
		return uri_valid(value);

	if (!strcmp(format, "date") || !strcmp(format, "date-time"))
		return date_valid(value, !strcmp(format, "date-time"));

	if (!strcmp(format, "email")) {
		const char *at = strrchr(value, '@');
		return at && at != value && at[1] && !strpbrk(value, "\r\n\t ") &&
		       strchr(value, '@') == at;
	}

	return 0;
}

static int field_value_valid(const cJSON *schema, const cJSON *value)
{
	const char *type = json_text(schema, "type");
	if (!strcmp(type, "string")) {
		if (!cJSON_IsString(value) || !value_in_enum(schema, value))
			return 0;

		size_t length = nb_utf8_count(nb_str_from_cstr(value->valuestring));
		return length != SIZE_MAX && string_format_valid(schema, value->valuestring) &&
		       within_bound(schema, "minLength", (double)length, 1) &&
		       within_bound(schema, "maxLength", (double)length, 0);
	}

	if (!strcmp(type, "boolean"))
		return cJSON_IsBool(value);

	if (!strcmp(type, "number") || !strcmp(type, "integer")) {
		return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
		       (strcmp(type, "integer") ||
			trunc(value->valuedouble) == value->valuedouble) &&
		       within_bound(schema, "minimum", value->valuedouble, 1) &&
		       within_bound(schema, "maximum", value->valuedouble, 0);
	}

	if (strcmp(type, "array") || !cJSON_IsArray(value))
		return 0;

	int count = cJSON_GetArraySize(value);
	if (!within_bound(schema, "minItems", count, 1) ||
	    !within_bound(schema, "maxItems", count, 0))
		return 0;

	const cJSON *items = cJSON_GetObjectItemCaseSensitive(schema, "items");
	const cJSON *item;
	cJSON_ArrayForEach (item, value) {
		if (!cJSON_IsString(item) || !value_in_enum(items, item))
			return 0;

		for (const cJSON *previous = value->child; previous != item;
		     previous = previous->next) {
			if (cJSON_Compare(previous, item, 1))
				return 0;
		}
	}

	return 1;
}

static int form_schema_valid(const cJSON *schema)
{
	const cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
	const cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
	if (strcmp(json_text(schema, "type"), "object") || !cJSON_IsObject(properties) ||
	    (required && !cJSON_IsArray(required)))
		return 0;

	const cJSON *field;
	cJSON_ArrayForEach (field, required) {
		if (!cJSON_IsString(field) || !cJSON_HasObjectItem(properties, field->valuestring))
			return 0;
	}

	cJSON_ArrayForEach (field, properties) {
		const char *type = json_text(field, "type");
		if (strcmp(type, "string") && strcmp(type, "number") && strcmp(type, "integer") &&
		    strcmp(type, "boolean") && strcmp(type, "array"))
			return 0;

		const char *minimums[] = {"minimum", "minLength", "minItems"};
		const char *maximums[] = {"maximum", "maxLength", "maxItems"};
		for (size_t i = 0; i < sizeof(minimums) / sizeof(minimums[0]); i++) {
			const cJSON *min = cJSON_GetObjectItemCaseSensitive(field, minimums[i]);
			const cJSON *max = cJSON_GetObjectItemCaseSensitive(field, maximums[i]);
			if ((min && (!cJSON_IsNumber(min) || !isfinite(min->valuedouble))) ||
			    (max && (!cJSON_IsNumber(max) || !isfinite(max->valuedouble))) ||
			    (min && max && min->valuedouble > max->valuedouble) ||
			    (i > 0 && ((min && (min->valuedouble < 0 ||
						trunc(min->valuedouble) != min->valuedouble)) ||
				       (max && (max->valuedouble < 0 ||
						trunc(max->valuedouble) != max->valuedouble)))))
				return 0;
		}

		const char *format = json_text(field, "format");
		if (format[0] && strcmp(format, "uri") && strcmp(format, "email") &&
		    strcmp(format, "date") && strcmp(format, "date-time"))
			return 0;

		const cJSON *options = !strcmp(type, "array")
					       ? cJSON_GetObjectItemCaseSensitive(field, "items")
					       : field;
		int count = enum_count(options);
		if (!strcmp(type, "array") && count == 0)
			return 0;

		for (int i = 0; i < count; i++) {
			if (!cJSON_IsString(enum_option(options, i)))
				return 0;
		}

		const cJSON *initial = cJSON_GetObjectItemCaseSensitive(field, "default");
		if (initial && !field_value_valid(field, initial))
			return 0;
	}

	return 1;
}

static int field_required(const cJSON *schema, const char *name)
{
	const cJSON *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
	const cJSON *item;

	cJSON_ArrayForEach (item, required) {
		if (cJSON_IsString(item) && !strcmp(item->valuestring, name))
			return 1;
	}

	return 0;
}

static int field_input(const struct tool_runtime_context *context, const char *question,
		       const cJSON *schema, cJSON **value)
{
	const char *type = json_text(schema, "type");
	int multiple = !strcmp(type, "array");
	const cJSON *items = multiple ? cJSON_GetObjectItemCaseSensitive(schema, "items") : schema;
	int count = enum_count(items);
	nb_buf prompt NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf_init(&prompt);
	(void)nb_buf_append_cstr(&prompt, question);
	for (int i = 0; i < count; i++) {
		const cJSON *option = cJSON_GetArrayItem(
			cJSON_GetObjectItemCaseSensitive(items, multiple ? "anyOf" : "oneOf"), i);
		const char *title = json_text(option, "title");
		const cJSON *names = cJSON_GetObjectItemCaseSensitive(items, "enumNames");
		const char *legacy = cJSON_GetStringValue(cJSON_GetArrayItem(names, i));
		if (title[0] || legacy || count > ASK_USER_CHOICES_MAX)
			(void)nb_buf_printf(&prompt, "\n%s: %s", enum_option(items, i)->valuestring,
					    title[0] ? title
					    : legacy ? legacy
						     : enum_option(items, i)->valuestring);
	}

	int free_enum = count > ASK_USER_CHOICES_MAX;
	if (free_enum && multiple)
		(void)nb_buf_append_cstr(&prompt, "\nEnter selected values as a JSON array.");

	if (!nb_buf_ok(&prompt))
		return nb_buf_status(&prompt);

	const char *choices[ASK_USER_CHOICES_MAX];
	int boolean = !strcmp(type, "boolean");
	if (boolean) {
		choices[0] = "Yes";
		choices[1] = "No";
		count = 2;
	} else {
		for (int i = 0; i < count && !free_enum; i++) {
			const cJSON *option = enum_option(items, i);
			if (!cJSON_IsString(option))
				return NB_ERR_INVAL;

			choices[i] = option->valuestring;
		}
	}

	if (multiple && count == 0)
		return NB_ERR_INVAL;

	struct elicitation_answers answers NB_CLEANUP(elicitation_answers_cleanup) = {0};
	int rc = elicit_ask(context, nb_buf_cstr(&prompt), choices, free_enum ? 0 : count,
			    multiple && !free_enum ? "multi" : "single",
			    multiple && !free_enum ? 0 : 1, multiple && !free_enum ? count : 1,
			    &answers);
	if (rc != 0)
		return rc;

	if (multiple && free_enum) {
		*value = cJSON_ParseWithOpts(answers.items[0], NULL, 1);
	} else if (multiple) {
		*value = cJSON_CreateArray();
		if (!*value)
			return NB_ERR_NOMEM;

		for (int i = 0; i < answers.count; i++) {
			if (!cJSON_AddItemToArray(*value, cJSON_CreateString(answers.items[i])))
				return NB_ERR_NOMEM;
		}
	} else if (boolean) {
		if (strcmp(answers.items[0], "Yes") && strcmp(answers.items[0], "No"))
			return NB_ERR_INVAL;

		*value = cJSON_CreateBool(!strcmp(answers.items[0], "Yes"));
	} else if (!strcmp(type, "string")) {
		*value = cJSON_CreateString(answers.items[0]);
	} else {
		*value = cJSON_ParseWithOpts(answers.items[0], NULL, 1);
	}

	return *value ? 0 : NB_ERR_INVAL;
}

static int collect_field(const struct tool_runtime_context *context, const char *heading,
			 const cJSON *schema, const cJSON *field, cJSON *content)
{
	nb_buf question NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf_init(&question);
	const char *title = json_text(field, "title");
	const char *description = json_text(field, "description");
	(void)nb_buf_printf(&question, "%s\n\n%s%s%s", heading, title[0] ? title : field->string,
			    description[0] ? "\n" : "", description);
	if (!nb_buf_ok(&question))
		return nb_buf_status(&question);

	const cJSON *initial = cJSON_GetObjectItemCaseSensitive(field, "default");
	int required = field_required(schema, field->string);
	cJSON *value NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	if (!required || initial || !strcmp(json_text(field, "type"), "string")) {
		const char *choices[4] = {"Enter a value"};
		int count = 1;
		if (!required)
			choices[count++] = "Omit this field";

		if (initial && field_value_valid(field, initial))
			choices[count++] = "Use the default value";

		if (!strcmp(json_text(field, "type"), "string"))
			choices[count++] = "Use empty text";

		struct elicitation_answers answers NB_CLEANUP(elicitation_answers_cleanup) = {0};
		int rc = elicit_ask(context, nb_buf_cstr(&question), choices, count, "single", 1, 1,
				    &answers);
		if (rc != 0)
			return rc;

		if (!strcmp(answers.items[0], "Omit this field") && !required)
			return 0;

		if (!strcmp(answers.items[0], "Use the default value") && initial)
			value = cJSON_Duplicate(initial, 1);
		else if (!strcmp(answers.items[0], "Use empty text"))
			value = cJSON_CreateString("");
	}

	while (!value || !field_value_valid(field, value)) {
		cJSON_DeleteOwner(&value);
		int rc = field_input(context, nb_buf_cstr(&question), field, &value);
		if (rc != 0 && rc != NB_ERR_INVAL)
			return rc;

		if (!value || !field_value_valid(field, value)) {
			(void)nb_buf_append_cstr(&question,
						 "\nThe value does not meet this field's "
						 "requirements. Please try again.");
			if (!nb_buf_ok(&question))
				return nb_buf_status(&question);
		}
	}

	if (!cJSON_AddItemToObject(content, field->string, value))
		return NB_ERR_NOMEM;

	value = NULL;
	return 0;
}

static int elicit_once(const char *server, const char *method, const cJSON *params,
		       const void *opaque, cJSON **result, void *user_data)
{
	(void)method;
	(void)user_data;
	const struct tool_runtime_context *context = opaque;
	const char *message = json_text(params, "message");
	const char *mode = json_text(params, "mode");
	int url_mode = !strcmp(mode, "url");
	if (!message[0] || (mode[0] && strcmp(mode, "form") && !url_mode))
		return NB_ERR_INVAL;

	if (!context || !context->ask_user_fn)
		return elicit_action("cancel", NULL, result);

	nb_buf heading NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf_init(&heading);
	(void)nb_buf_printf(&heading, "MCP server: %s\n%s", server, message);
	cJSON *content NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
	if (!content)
		return NB_ERR_NOMEM;

	if (url_mode) {
		const char *url = json_text(params, "url");
		const char *authority = strstr(url, "://");
		if ((strncasecmp(url, "https://", 8) && strncasecmp(url, "http://", 7)) ||
		    !uri_valid(url) || !authority || !authority[3] || authority[3] == '/' ||
		    authority[3] == '#' || authority[3] == '?')
			return NB_ERR_INVAL;

		(void)nb_buf_printf(&heading,
				    "\n\nOpen this address in your browser to continue:\n%s\n"
				    "Confirm only after completing the requested interaction.",
				    url);
	} else {
		const cJSON *schema = cJSON_GetObjectItemCaseSensitive(params, "requestedSchema");
		const cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
		if (!form_schema_valid(schema))
			return NB_ERR_INVAL;

		const cJSON *field;
		cJSON_ArrayForEach (field, properties) {
			int rc = collect_field(context, nb_buf_cstr(&heading), schema, field,
					       content);
			if (rc == NB_ERR_CANCELLED || rc == NB_ERR_INTERRUPTED)
				return elicit_action("cancel", NULL, result);

			if (rc != 0)
				return rc;
		}

		if (content->child) {
			char *summary NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_Print(content);
			if (!summary)
				return NB_ERR_NOMEM;

			(void)nb_buf_printf(&heading, "\n\nReview your answers:\n%s", summary);
		}
	}

	if (!nb_buf_ok(&heading))
		return nb_buf_status(&heading);

	const char *choices[] = {"Accept", "Decline", "Cancel", "Edit answers"};
	struct elicitation_answers answers NB_CLEANUP(elicitation_answers_cleanup) = {0};
	int rc = elicit_ask(context, nb_buf_cstr(&heading), choices,
			    !url_mode && content->child ? 4 : 3, "single", 1, 1, &answers);
	if (rc == NB_ERR_CANCELLED || rc == NB_ERR_INTERRUPTED)
		return elicit_action("cancel", NULL, result);

	if (rc != 0)
		return rc;

	const char *choice = answers.items[0];
	if (!strcmp(choice, "Edit answers") && !url_mode && content->child) {
		/* A fresh form keeps defaults and validation identical during editing. */
		return NB_ERR_AGAIN;
	}

	if (strcmp(choice, "Accept") && strcmp(choice, "Decline") && strcmp(choice, "Cancel"))
		return NB_ERR_INVAL;

	return elicit_action(!strcmp(choice, "Accept")	  ? "accept"
			     : !strcmp(choice, "Decline") ? "decline"
							  : "cancel",
			     url_mode ? NULL : &content, result);
}

int runtime_mcp_elicitation(const char *server, const char *method, const cJSON *params,
			    const void *context, cJSON **result, void *user_data)
{
	if (!server || !params || !result)
		return NB_ERR_INVAL;

	if (*result)
		return NB_ERR_EXISTS;

	for (;;) {
		int rc = elicit_once(server, method, params, context, result, user_data);
		if (rc != NB_ERR_AGAIN)
			return rc;
	}
}
