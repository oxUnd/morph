#include "runtime/services.h"
#include "runtime/runtime_internal.h"
#include "util/arena.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static struct mcp_client *runtime_mcp_lookup_client(struct runtime *runtime,
						    const char *name)
{
	return runtime && name ? mcp_registry_get(&runtime->context.mcp, name)
		: NULL;
}

static void runtime_mcp_status_copy(const struct mcp_client *client,
				    struct runtime_mcp_status *out)
{
	memset(out, 0, sizeof(*out));
	out->config = client->config;
	out->connected = client->connected;
	strncpy(out->negotiated_version, client->negotiated_version,
		sizeof(out->negotiated_version) - 1);
	strncpy(out->server_name, client->server_name,
		sizeof(out->server_name) - 1);
	strncpy(out->server_version, client->server_version,
		sizeof(out->server_version) - 1);
	out->supports_tools = client->supports_tools;
	out->supports_resources = client->supports_resources;
	out->supports_prompts = client->supports_prompts;
}

int runtime_mcp_count(const struct runtime *runtime)
{
	return runtime ? runtime->context.mcp.count : 0;
}

int runtime_mcp_info(const struct runtime *runtime, int index,
		     struct runtime_mcp_status *out)
{
	if (!runtime || !out || index < 0 || index >= runtime->context.mcp.count)
		return -EINVAL;
	runtime_mcp_status_copy(runtime->context.mcp.servers[index], out);
	return 0;
}

int runtime_mcp_find(const struct runtime *runtime, const char *name,
		     struct runtime_mcp_status *out)
{
	struct mcp_client *client;
	if (!runtime || !name || !out)
		return -EINVAL;
	client = mcp_registry_get((struct mcp_registry *)&runtime->context.mcp,
				  name);
	if (!client)
		return -ENOENT;
	runtime_mcp_status_copy(client, out);
	return 0;
}

int runtime_mcp_connect(struct runtime *runtime, const char *name)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	return client ? mcp_ensure_connected(client) : -ENOENT;
}

int runtime_mcp_disconnect(struct runtime *runtime, const char *name)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	if (!client)
		return -ENOENT;
	mcp_disconnect(client);
	return 0;
}

static int runtime_mcp_copy_list(void **out, int count, const void *items,
				 size_t item_size)
{
	*out = NULL;
	if (count <= 0)
		return 0;
	*out = malloc((size_t)count * item_size);
	if (!*out)
		return -ENOMEM;
	memcpy(*out, items, (size_t)count * item_size);
	return 0;
}

int runtime_mcp_list_tools(struct runtime *runtime, const char *name,
			   struct mcp_tool_desc **out, int *count)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	struct mcp_tool_desc *items = NULL;
	struct arena *arena;
	int rc;
	if (!client || !out || !count)
		return client ? -EINVAL : -ENOENT;
	arena = arena_create(0);
	if (!arena)
		return -ENOMEM;
	rc = mcp_ensure_connected(client);
	if (rc == 0)
		rc = mcp_list_tools(client, arena, &items, count);
	if (rc == 0)
		rc = runtime_mcp_copy_list((void **)out, *count, items,
					   sizeof(**out));
	arena_destroy(arena);
	return rc;
}

int runtime_mcp_list_resources(struct runtime *runtime, const char *name,
			       struct mcp_resource_desc **out, int *count)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	struct mcp_resource_desc *items = NULL;
	struct arena *arena;
	int rc;
	if (!client || !out || !count)
		return client ? -EINVAL : -ENOENT;
	arena = arena_create(0);
	if (!arena)
		return -ENOMEM;
	rc = mcp_ensure_connected(client);
	if (rc == 0)
		rc = mcp_list_resources(client, arena, &items, count);
	if (rc == 0)
		rc = runtime_mcp_copy_list((void **)out, *count, items,
					   sizeof(**out));
	arena_destroy(arena);
	return rc;
}

int runtime_mcp_list_prompts(struct runtime *runtime, const char *name,
			     struct mcp_prompt_desc **out, int *count)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	struct mcp_prompt_desc *items = NULL;
	struct arena *arena;
	int rc;
	if (!client || !out || !count)
		return client ? -EINVAL : -ENOENT;
	arena = arena_create(0);
	if (!arena)
		return -ENOMEM;
	rc = mcp_ensure_connected(client);
	if (rc == 0)
		rc = mcp_list_prompts(client, arena, &items, count);
	if (rc == 0)
		rc = runtime_mcp_copy_list((void **)out, *count, items,
					   sizeof(**out));
	arena_destroy(arena);
	return rc;
}

void runtime_mcp_list_free(void *items)
{
	free(items);
}

int runtime_mcp_discover(struct runtime *runtime, const char *name,
			 int *tools, int *resources, int *prompts)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	int t;
	int r;
	int p;
	if (!client)
		return -ENOENT;
	t = mcp_register_server_tools(client, &runtime->context.tools);
	r = mcp_register_server_resources(client, &runtime->context.tools);
	p = mcp_register_server_prompts(client, &runtime->context.tools);
	if (tools)
		*tools = t;
	if (resources)
		*resources = r;
	if (prompts)
		*prompts = p;
	return t < 0 ? t : (r < 0 ? r : (p < 0 ? p : 0));
}
