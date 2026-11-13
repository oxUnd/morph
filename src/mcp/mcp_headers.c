#include "mcp_internal.h"
#include "nb/encoding.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "errors.h"

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

int mcp_protocol_add_header_copy(nb_vec *headers, const char *name, const char *value)
{
	if (!headers || headers->elem_size != sizeof(char *) || !name || !value)
		return NB_ERR_INVAL;
	nb_buf buffer NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf encoded_bytes NB_CLEANUP(nb_buf_destroy) = {0};
	const char *encoded;
	int rc = (nb_buf_init(&buffer), nb_buf_reserve(&buffer, 128) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	if (header_needs_encoding(value)) {
		if (nb_base64_encode((const unsigned char *)value, strlen(value), NB_B64_STD, true,
				     &encoded_bytes, NULL) != NB_OK)
			return NB_ERR_NOMEM;
		encoded = nb_buf_cstr(&encoded_bytes);
		rc = (nb_buf_printf(&buffer, "%s: =?base64?%s?=", name, encoded) ? 0
										 : NB_ERR_NOMEM);
	} else {
		rc = (nb_buf_printf(&buffer, "%s: %s", name, value) ? 0 : NB_ERR_NOMEM);
	}
	if (rc != 0)
		return rc;
	char *text NB_CLEANUP(nb_charp_cleanup) =
		(nb_buf_ok(&buffer) ? strdup(nb_buf_cstr(&buffer)) : NULL);
	if (!text)
		return NB_ERR_NOMEM;
	char **slot = nb_vec_push(headers);

	if (!slot)
		return NB_ERR_NOMEM;
	*slot = nb_charp_steal(&text);
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

static int validate_annotation(const cJSON *schema, int property, nb_map *seen)
{
	const cJSON *header = cJSON_GetObjectItemCaseSensitive(schema, "x-mcp-header");

	if (!header)
		return 0;
	const cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");

	if (!property || !cJSON_IsString(header) || !header_token_valid(header->valuestring) ||
	    !cJSON_IsString(type) || (strcmp(type->valuestring, "string") != 0 &&
	    strcmp(type->valuestring, "integer") != 0 && strcmp(type->valuestring, "boolean") != 0))
		return NB_ERR_INVAL;
	char *key = strdup(header->valuestring);

	if (!key)
		return NB_ERR_NOMEM;
	for (char *p = key; *p; p++) {
		if (*p >= 'A' && *p <= 'Z')
			*p = (char)(*p - 'A' + 'a');
	}
	int rc = nb_map_has_cstr(seen, key)
			 ? NB_ERR_INVAL
			 : (nb_map_put_cstr(seen, key, seen, NULL) ? 0 : NB_ERR_NOMEM);

	free(key);
	return rc;
}

/* Only properties chains can emit headers; other schema positions are checked
 * so annotations hidden under $ref/composition/arrays are rejected as specified.
 * Instance values in enum/const/default/examples are data, not schemas. */
static int validate_schema(const cJSON *schema, int reachable, int property, nb_map *seen)
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

int mcp_protocol_cache_tool_copy(struct mcp_client *client, const char *name, const cJSON *schema)
{
	if (!client->protocol || !client->protocol->per_request ||
	    client->config.transport != MCP_TRANSPORT_STREAMABLE_HTTP)
		return 0;
	nb_map *seen = nb_map_new_with_cap(8);
	int rc = seen ? 0 : NB_ERR_NOMEM;

	if (rc != 0)
		return rc;
	rc = validate_schema(schema, 1, 0, seen);
	nb_map_free(seen);
	if (rc != 0)
		return rc;
	cJSON *copy = cJSON_Duplicate(schema, 1);

	if (!copy)
		return NB_ERR_NOMEM;
	nb_mutex_lock(client->lock);
	if (!client->tool_schemas) {
		client->tool_schemas = nb_map_new_with_cap(16);
		if (!client->tool_schemas)
			rc = NB_ERR_NOMEM;
	}
	cJSON *previous = nb_map_get_cstr(client->tool_schemas, name);

	if (rc == 0)
		rc = nb_map_put_cstr(client->tool_schemas, name, copy, NULL) ? 0 : NB_ERR_NOMEM;
	if (rc == 0)
		cJSON_Delete(previous);
	else
		cJSON_Delete(copy);
	nb_mutex_unlock(client->lock);
	return rc;
}

static int parameter_header(const cJSON *schema, const cJSON *value, nb_vec *headers)
{
	const cJSON *annotation = cJSON_GetObjectItemCaseSensitive(schema, "x-mcp-header");

	if (!annotation || !value || cJSON_IsNull(value))
		return 0;
	const cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");
	nb_buf name NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf text NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = (nb_buf_init(&name), nb_buf_reserve(&name, 64) ? 0 : NB_ERR_NOMEM);

	if (rc != 0)
		return rc;
	rc = (nb_buf_printf(&name, "Mcp-Param-%s", annotation->valuestring) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	rc = (nb_buf_init(&text), nb_buf_reserve(&text, 64) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	if (strcmp(type->valuestring, "string") == 0 && cJSON_IsString(value))
		rc = (nb_buf_append_cstr(&text, value->valuestring) ? 0 : NB_ERR_NOMEM);
	else if (strcmp(type->valuestring, "boolean") == 0 && cJSON_IsBool(value))
		rc = (nb_buf_append_cstr(&text, cJSON_IsTrue(value) ? "true" : "false")
			      ? 0
			      : NB_ERR_NOMEM);
	else if (strcmp(type->valuestring, "integer") == 0 && cJSON_IsNumber(value) &&
	    value->valuedouble >= -9007199254740991.0 &&
	    value->valuedouble <= 9007199254740991.0 &&
	    (double)(int64_t)value->valuedouble == value->valuedouble)
		rc = (nb_buf_printf(&text, "%.0f", value->valuedouble) ? 0 : NB_ERR_NOMEM);
	else
		return NB_ERR_INVAL;
	if (rc != 0)
		return rc;
	return mcp_protocol_add_header_copy(headers, nb_buf_cstr(&name), nb_buf_cstr(&text));
}

static int property_headers(const cJSON *schema, const cJSON *arguments, nb_vec *headers)
{
	const cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
	const cJSON *property;

	cJSON_ArrayForEach(property, properties) {
		const cJSON *value = cJSON_IsObject(arguments) ?
			cJSON_GetObjectItemCaseSensitive(arguments, property->string) : NULL;
		int rc = parameter_header(property, value, headers);

		if (rc == 0 && cJSON_IsObject(value))
			rc = property_headers(property, value, headers);
		if (rc != 0)
			return rc;
	}
	return 0;
}

static void header_copies_cleanup(nb_vec *headers)
{
	char **header;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(headers) && (header = (char **)nb_vec_at(headers, _i_)); _i_++)
		free(*header);
	nb_vec_cleanup(headers);
}

/* Caller holds the connection lock, including access to the schema cache. */
int mcp_protocol_tool_headers_copy(struct mcp_client *client, const cJSON *params, nb_vec *headers)
{
	if (!client || !params || !headers || headers->elem_size != sizeof(char *))
		return NB_ERR_INVAL;
	const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
	const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
	const cJSON *schema = cJSON_IsString(name)
				      ? nb_map_get_cstr(client->tool_schemas, name->valuestring)
				      : NULL;

	if (!schema)
		return NB_ERR_NOT_FOUND;
	nb_vec copies NB_CLEANUP(header_copies_cleanup) = {0};
	int rc = (nb_vec_init(&copies, sizeof(char *)),
		  nb_vec_reserve(&copies, 8) ? 0 : NB_ERR_NOMEM);
	if (rc == 0)
		rc = property_headers(schema, arguments, &copies);
	if (rc != 0 || !copies.len)
		return rc;
	if (copies.len > SIZE_MAX - headers->len)
		return NB_ERR_RANGE;
	char **destination = nb_vec_push_n(headers, copies.len);
	if (!destination)
		return NB_ERR_NOMEM;
	memcpy(destination, copies.data, copies.len * sizeof(*destination));
	copies.len = 0;
	return 0;
}
