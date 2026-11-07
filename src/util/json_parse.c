#define _GNU_SOURCE
#include "json_internal.h"

#include "utf8.h"

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif

struct json_parser {
	const char *text;
	size_t length;
	size_t position;
	size_t error_offset;
	int error;
};

static cJSON *parse_value(struct json_parser *parser, unsigned depth);

static void parse_error(struct json_parser *parser, int error)
{
	if (!parser->error) {
		parser->error = error;
		parser->error_offset = parser->position;
	}
}

static void skip_whitespace(struct json_parser *parser)
{
	while (parser->position < parser->length) {
		char ch = parser->text[parser->position];

		if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
			break;
		parser->position++;
	}
}

static int parse_hex_quad(struct json_parser *parser, unsigned *value)
{
	*value = 0;
	for (unsigned i = 0; i < 4; i++) {
		unsigned digit;
		char ch;

		if (parser->position == parser->length) {
			parse_error(parser, -EINVAL);
			return -EINVAL;
		}
		ch = parser->text[parser->position];
		if (ch >= '0' && ch <= '9')
			digit = (unsigned)(ch - '0');
		else if (ch >= 'a' && ch <= 'f')
			digit = (unsigned)(ch - 'a') + 10;
		else if (ch >= 'A' && ch <= 'F')
			digit = (unsigned)(ch - 'A') + 10;
		else {
			parse_error(parser, -EINVAL);
			return -EINVAL;
		}
		*value = (*value << 4) | digit;
		parser->position++;
	}
	return 0;
}

static int parse_escape(struct json_parser *parser)
{
	unsigned value;
	unsigned low;
	char ch;

	if (parser->position == parser->length) {
		parse_error(parser, -EINVAL);
		return -EINVAL;
	}
	ch = parser->text[parser->position++];
	if (ch == '"' || ch == '\\' || ch == '/' || ch == 'b' || ch == 'f' || ch == 'n' ||
	    ch == 'r' || ch == 't')
		return 0;
	if (ch != 'u') {
		parser->position--;
		parse_error(parser, -EINVAL);
		return -EINVAL;
	}
	if (parse_hex_quad(parser, &value) != 0)
		return parser->error;
	if (!value) {
		parse_error(parser, -ENOTSUP);
		return -ENOTSUP;
	}
	if (value >= 0xDC00 && value <= 0xDFFF) {
		parse_error(parser, -EINVAL);
		return -EINVAL;
	}
	if (value < 0xD800 || value > 0xDBFF)
		return 0;
	if (parser->length - parser->position < 2 || parser->text[parser->position] != '\\' ||
	    parser->text[parser->position + 1] != 'u') {
		parse_error(parser, -EINVAL);
		return -EINVAL;
	}
	parser->position += 2;
	if (parse_hex_quad(parser, &low) != 0)
		return parser->error;
	if (low < 0xDC00 || low > 0xDFFF) {
		parse_error(parser, -EINVAL);
		return -EINVAL;
	}
	return 0;
}

static cJSON *parse_string(struct json_parser *parser)
{
	size_t start = parser->position++;
	const char *end;
	cJSON *value;

	while (parser->position < parser->length) {
		unsigned char ch = (unsigned char)parser->text[parser->position];

		if (ch == '"') {
			parser->position++;
			value = cJSON_ParseWithLengthOpts(parser->text + start,
							  parser->position - start, &end, 0);
			if (!value)
				parse_error(parser, -ENOMEM);
			return value;
		}
		if (ch < 0x20) {
			parse_error(parser, ch ? -EINVAL : -ENOTSUP);
			return NULL;
		}
		if (ch == '\\') {
			parser->position++;
			if (parse_escape(parser) != 0)
				return NULL;
		} else if (ch >= 0x80) {
			unsigned codepoint;
			size_t bytes;

			if (!utf8_decode_codepoint(parser->text + parser->position,
						   parser->length - parser->position, &codepoint,
						   &bytes)) {
				parse_error(parser, -EILSEQ);
				return NULL;
			}
			parser->position += bytes;
		} else {
			parser->position++;
		}
	}
	parse_error(parser, -EINVAL);
	return NULL;
}

static int is_digit(char ch)
{
	return ch >= '0' && ch <= '9';
}

static cJSON *parse_number(struct json_parser *parser)
{
	size_t start = parser->position;
	char *number;
	char *end;
	double parsed;
	locale_t locale;
	cJSON *value;

	if (parser->text[parser->position] == '-')
		parser->position++;
	if (parser->position == parser->length || !is_digit(parser->text[parser->position])) {
		parse_error(parser, -EINVAL);
		return NULL;
	}
	if (parser->text[parser->position] == '0') {
		parser->position++;
		if (parser->position < parser->length && is_digit(parser->text[parser->position])) {
			parse_error(parser, -EINVAL);
			return NULL;
		}
	} else {
		while (parser->position < parser->length &&
		       is_digit(parser->text[parser->position]))
			parser->position++;
	}
	if (parser->position < parser->length && parser->text[parser->position] == '.') {
		parser->position++;
		if (parser->position == parser->length ||
		    !is_digit(parser->text[parser->position])) {
			parse_error(parser, -EINVAL);
			return NULL;
		}
		while (parser->position < parser->length &&
		       is_digit(parser->text[parser->position]))
			parser->position++;
	}
	if (parser->position < parser->length &&
	    (parser->text[parser->position] == 'e' || parser->text[parser->position] == 'E')) {
		parser->position++;
		if (parser->position < parser->length && (parser->text[parser->position] == '+' ||
							  parser->text[parser->position] == '-'))
			parser->position++;
		if (parser->position == parser->length ||
		    !is_digit(parser->text[parser->position])) {
			parse_error(parser, -EINVAL);
			return NULL;
		}
		while (parser->position < parser->length &&
		       is_digit(parser->text[parser->position]))
			parser->position++;
	}
	number = malloc(parser->position - start + 1);
	if (!number) {
		parse_error(parser, -ENOMEM);
		return NULL;
	}
	memcpy(number, parser->text + start, parser->position - start);
	number[parser->position - start] = '\0';
	locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
	if (!locale) {
		int rc = errno ? -errno : -ENOMEM;

		free(number);
		parse_error(parser, rc);
		return NULL;
	}
	parsed = strtod_l(number, &end, locale);
	int valid = *end == '\0' && isfinite(parsed);

	if (parsed == 0) {
		for (const char *digit = number; *digit && *digit != 'e' && *digit != 'E';
		     digit++) {
			if (*digit >= '1' && *digit <= '9') {
				valid = 0;
				break;
			}
		}
	}

	freelocale(locale);
	free(number);
	if (!valid) {
		parse_error(parser, -ERANGE);
		return NULL;
	}
	value = cJSON_CreateNumber(parsed);
	if (!value)
		parse_error(parser, -ENOMEM);
	return value;
}

static cJSON *parse_container(struct json_parser *parser, unsigned depth, int object)
{
	cJSON *container;
	char closing = object ? '}' : ']';

	if (depth >= JSON_MAX_DEPTH) {
		parse_error(parser, -E2BIG);
		return NULL;
	}
	container = object ? cJSON_CreateObject() : cJSON_CreateArray();
	if (!container) {
		parse_error(parser, -ENOMEM);
		return NULL;
	}
	parser->position++;
	skip_whitespace(parser);
	if (parser->position < parser->length && parser->text[parser->position] == closing) {
		parser->position++;
		return container;
	}
	for (;;) {
		cJSON *key = NULL;
		cJSON *value;
		int added;

		if (object) {
			if (parser->position == parser->length ||
			    parser->text[parser->position] != '"') {
				parse_error(parser, -EINVAL);
				break;
			}
			key = parse_string(parser);
			if (!key)
				break;
			skip_whitespace(parser);
			if (parser->position == parser->length ||
			    parser->text[parser->position] != ':') {
				cJSON_Delete(key);
				parse_error(parser, -EINVAL);
				break;
			}
			parser->position++;
		}
		skip_whitespace(parser);
		value = parse_value(parser, depth + 1);
		if (!value) {
			cJSON_Delete(key);
			break;
		}
		added = object ? cJSON_AddItemToObject(container, key->valuestring, value)
			       : cJSON_AddItemToArray(container, value);
		cJSON_Delete(key);
		if (!added) {
			cJSON_Delete(value);
			parse_error(parser, -ENOMEM);
			break;
		}
		skip_whitespace(parser);
		if (parser->position == parser->length) {
			parse_error(parser, -EINVAL);
			break;
		}
		if (parser->text[parser->position] == closing) {
			parser->position++;
			return container;
		}
		if (parser->text[parser->position] != ',') {
			parse_error(parser, -EINVAL);
			break;
		}
		parser->position++;
		skip_whitespace(parser);
	}
	cJSON_Delete(container);
	return NULL;
}

static cJSON *parse_value(struct json_parser *parser, unsigned depth)
{
	cJSON *value;
	const char *literal;
	size_t length;
	char ch;

	if (depth >= JSON_MAX_DEPTH) {
		parse_error(parser, -E2BIG);
		return NULL;
	}
	if (parser->position == parser->length) {
		parse_error(parser, -EINVAL);
		return NULL;
	}
	ch = parser->text[parser->position];
	if (ch == '{' || ch == '[')
		return parse_container(parser, depth, ch == '{');
	if (ch == '"')
		return parse_string(parser);
	if (ch == '-' || is_digit(ch))
		return parse_number(parser);
	if (ch == 't')
		literal = "true";
	else if (ch == 'f')
		literal = "false";
	else if (ch == 'n')
		literal = "null";
	else {
		parse_error(parser, ch ? -EINVAL : -ENOTSUP);
		return NULL;
	}
	length = strlen(literal);
	if (parser->length - parser->position < length ||
	    memcmp(parser->text + parser->position, literal, length) != 0) {
		parse_error(parser, -EINVAL);
		return NULL;
	}
	parser->position += length;
	value = ch == 'n' ? cJSON_CreateNull() : cJSON_CreateBool(ch == 't');
	if (!value)
		parse_error(parser, -ENOMEM);
	return value;
}

int json_parse_backend(const char *text, size_t length, int prefix, cJSON **out, size_t *consumed,
		       size_t *error_offset)
{
	struct json_parser parser = {.text = text, .length = length};
	cJSON *value;

	if (out)
		*out = NULL;
	if (consumed)
		*consumed = 0;
	if (error_offset)
		*error_offset = 0;
	if (!text || !out)
		return -EINVAL;
	skip_whitespace(&parser);
	value = parse_value(&parser, 0);
	if (value && !prefix) {
		skip_whitespace(&parser);
		if (parser.position != parser.length) {
			parse_error(&parser, parser.text[parser.position] ? -EINVAL : -ENOTSUP);
			cJSON_Delete(value);
			value = NULL;
		}
	}
	if (parser.error) {
		if (error_offset)
			*error_offset = parser.error_offset;
		return parser.error;
	}
	*out = value;
	if (consumed)
		*consumed = parser.position;
	return 0;
}
