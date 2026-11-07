#include "mcp_internal.h"
#include "util/base64.h"
#include "util/buf.h"
#include "util/error.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Header values are byte strings here; UTF-8 is preserved by Base64 encoding. */
static int header_needs_encoding(const char *value)
{
	size_t length = strlen(value);

	/* libcurl treats a plain empty field as header removal. Encode it explicitly. */
	if (length == 0)
		return 1;
	if (length && (value[0] == ' ' || value[0] == '\t' ||
	    value[length - 1] == ' ' || value[length - 1] == '\t'))
		return 1;
	if (length >= 10 && strncmp(value, "=?base64?", 8) == 0 &&
	    strcmp(value + length - 2, "?=") == 0)
		return 1;
	for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
		if ((*p < 0x20 && *p != '\t') || *p > 0x7e)
			return 1;
	}
	return 0;
}

int mcp_protocol_add_header(struct arena *arena, morph_array_t *headers,
	const char *name, const char *value)
{
	morph_buf_t buffer;
	char *encoded = NULL;
	int rc = morph_buf_init_arena(&buffer, arena, 128);

	if (rc != 0)
		return rc;
	if (header_needs_encoding(value)) {
		encoded = base64_encode((const unsigned char *)value, strlen(value));
		if (!encoded)
			return -ENOMEM;
		rc = morph_buf_printf(&buffer, "%s: =?base64?%s?=", name, encoded);
	} else {
		rc = morph_buf_printf(&buffer, "%s: %s", name, value);
	}
	free(encoded);
	if (rc != 0)
		return rc;
	const char **slot = morph_array_push(headers);

	if (!slot)
		return -ENOMEM;
	*slot = morph_buf_cstr(&buffer);
	return 0;
}

static int header_token_valid(const char *name)
{
	if (!name[0])
		return 0;
	for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || strchr("!#$%&'*+-.^_`|~", *p))
			continue;
		return 0;
	}
	return 1;
}

static int validate_annotation(const cJSON *schema, int property,
	morph_strmap_t *seen)
{
	const cJSON *header = cJSON_GetObjectItemCaseSensitive(schema, "x-mcp-header");

	if (!header)
		return 0;
	const cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");

	if (!property || !cJSON_IsString(header) || !header_token_valid(header->valuestring) ||
	    !cJSON_IsString(type) || (strcmp(type->valuestring, "string") != 0 &&
	    strcmp(type->valuestring, "integer") != 0 && strcmp(type->valuestring, "boolean") != 0))
		return -EINVAL;
	char *key = strdup(header->valuestring);

	if (!key)
		return -ENOMEM;
	for (char *p = key; *p; p++) {
		if (*p >= 'A' && *p <= 'Z')
			*p = (char)(*p - 'A' + 'a');
	}
	int rc = morph_strmap_contains(seen, key) ? -EINVAL :
		morph_strmap_set(seen, key, seen);

	free(key);
	return rc;
}

/* Only properties chains can emit headers; other schema positions are checked
 * so annotations hidden under $ref/composition/arrays are rejected as specified.
 * Instance values in enum/const/default/examples are data, not schemas. */
static int validate_schema(const cJSON *schema, int reachable, int property,
	morph_strmap_t *seen)
{
	if (!cJSON_IsObject(schema))
		return 0;
	int rc = validate_annotation(schema, reachable && property, seen);

	if (rc != 0)
		return rc;
	const cJSON *item;

	cJSON_ArrayForEach(item, schema) {
		const char *key = item->string;
		int properties = strcmp(key, "properties") == 0;
		int map = properties || strcmp(key, "patternProperties") == 0 ||
			strcmp(key, "$defs") == 0 || strcmp(key, "definitions") == 0 ||
			strcmp(key, "dependentSchemas") == 0;
		int list = strcmp(key, "allOf") == 0 || strcmp(key, "anyOf") == 0 ||
			strcmp(key, "oneOf") == 0 || strcmp(key, "prefixItems") == 0;
		int single = strcmp(key, "items") == 0 || strcmp(key, "contains") == 0 ||
			strcmp(key, "not") == 0 || strcmp(key, "if") == 0 ||
			strcmp(key, "then") == 0 || strcmp(key, "else") == 0 ||
			strcmp(key, "additionalProperties") == 0 ||
			strcmp(key, "unevaluatedProperties") == 0 ||
			strcmp(key, "unevaluatedItems") == 0 || strcmp(key, "propertyNames") == 0;

		if (map || list) {
			const cJSON *child;

			cJSON_ArrayForEach(child, item) {
				rc = validate_schema(child, reachable && properties,
					properties, seen);
				if (rc != 0)
					return rc;
			}
		} else if (single) {
			rc = validate_schema(item, 0, 0, seen);
			if (rc != 0)
				return rc;
		}
	}
	return 0;
}

int mcp_protocol_cache_tool(struct mcp_client *client, const char *name,
	const cJSON *schema)
{
	if (!client->protocol || !client->protocol->per_request ||
	    client->config.transport != MCP_TRANSPORT_STREAMABLE_HTTP)
		return 0;
	morph_strmap_t seen;
	int rc = morph_strmap_init(&seen, 8);

	if (rc != 0)
		return rc;
	rc = validate_schema(schema, 1, 0, &seen);
	morph_strmap_cleanup(&seen);
	if (rc != 0)
		return rc;
	cJSON *copy = cJSON_Duplicate(schema, 1);

	if (!copy)
		return -ENOMEM;
	pthread_mutex_lock(&client->lock);
	if (!client->tool_schemas.entries)
		rc = morph_strmap_init(&client->tool_schemas, 16);
	cJSON *previous = morph_strmap_get(&client->tool_schemas, name);

	if (rc == 0)
		rc = morph_strmap_set(&client->tool_schemas, name, copy);
	if (rc == 0)
		cJSON_Delete(previous);
	else
		cJSON_Delete(copy);
	pthread_mutex_unlock(&client->lock);
	return rc;
}

static int parameter_header(const cJSON *schema, const cJSON *value,
	struct arena *arena, morph_array_t *headers)
{
	const cJSON *annotation = cJSON_GetObjectItemCaseSensitive(schema, "x-mcp-header");

	if (!annotation || !value || cJSON_IsNull(value))
		return 0;
	const cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");
	morph_buf_t name;
	morph_buf_t text;
	int rc = morph_buf_init_arena(&name, arena, 64);

	if (rc != 0)
		return rc;
	rc = morph_buf_printf(&name, "Mcp-Param-%s", annotation->valuestring);
	if (rc != 0)
		return rc;
	rc = morph_buf_init_arena(&text, arena, 64);
	if (rc != 0)
		return rc;
	if (strcmp(type->valuestring, "string") == 0 && cJSON_IsString(value))
		rc = morph_buf_puts(&text, value->valuestring);
	else if (strcmp(type->valuestring, "boolean") == 0 && cJSON_IsBool(value))
		rc = morph_buf_puts(&text, cJSON_IsTrue(value) ? "true" : "false");
	else if (strcmp(type->valuestring, "integer") == 0 && cJSON_IsNumber(value) &&
	    value->valuedouble >= -9007199254740991.0 &&
	    value->valuedouble <= 9007199254740991.0 &&
	    (double)(int64_t)value->valuedouble == value->valuedouble)
		rc = morph_buf_printf(&text, "%.0f", value->valuedouble);
	else
		return -EINVAL;
	if (rc != 0)
		return rc;
	return mcp_protocol_add_header(arena, headers, morph_buf_cstr(&name),
		morph_buf_cstr(&text));
}

static int property_headers(const cJSON *schema, const cJSON *arguments,
	struct arena *arena, morph_array_t *headers)
{
	const cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
	const cJSON *property;

	cJSON_ArrayForEach(property, properties) {
		const cJSON *value = cJSON_IsObject(arguments) ?
			cJSON_GetObjectItemCaseSensitive(arguments, property->string) : NULL;
		int rc = parameter_header(property, value, arena, headers);

		if (rc == 0 && cJSON_IsObject(value))
			rc = property_headers(property, value, arena, headers);
		if (rc != 0)
			return rc;
	}
	return 0;
}

/* Caller holds the connection lock, including access to the schema cache. */
int mcp_protocol_tool_headers(struct mcp_client *client, const cJSON *params,
	struct arena *arena, morph_array_t *headers)
{
	const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
	const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
	const cJSON *schema = cJSON_IsString(name) ?
		morph_strmap_get(&client->tool_schemas, name->valuestring) : NULL;

	if (!schema)
		return -ENOENT;
	return property_headers(schema, arguments, arena, headers);
}
