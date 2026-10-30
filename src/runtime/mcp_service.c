#include "runtime/services.h"
#include "runtime/mcp.h"
#include "runtime/runtime_internal.h"
#include "util/arena.h"
#include "util/error.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static struct mcp_client *runtime_mcp_lookup_client(struct runtime *runtime,
						    const char *name)
{
	return runtime && name ? mcp_registry_get(&runtime->context.mcp, name)
		: NULL;
}

static int runtime_mcp_busy(struct runtime *runtime, struct mcp_client *client)
{
	struct runtime_mcp_job *job = runtime_mcp_job_find(runtime, client);
	int active = 0;

	if (job) {
		pthread_mutex_lock(&job->lock);
		active = job->active;
		pthread_mutex_unlock(&job->lock);
		if (!active && job->started && !pthread_equal(pthread_self(), job->thread)) {
			pthread_join(job->thread, NULL);
			job->started = 0;
		}
	}
	return active;
}

static void runtime_mcp_update_state(struct runtime *runtime,
	struct mcp_client *client, enum runtime_mcp_state state, int error_code)
{
	struct runtime_mcp_job *job = runtime_mcp_job_find(runtime, client);

	if (job) {
		pthread_mutex_lock(&job->lock);
		job->state = state;
		job->error_code = error_code;
		pthread_mutex_unlock(&job->lock);
	}
}

static void runtime_mcp_status_copy(struct runtime *runtime,
				    struct mcp_client *client,
				    struct runtime_mcp_status *out)
{
	struct runtime_mcp_job *job = runtime_mcp_job_find(runtime, client);

	memset(out, 0, sizeof(*out));
	out->config = client->config;
	if (job) {
		pthread_mutex_lock(&job->lock);
		out->state = job->state;
		out->error_code = job->error_code;
		out->elapsed_ms = job->active ? runtime_mcp_now_ms() - job->started_ms :
			job->elapsed_ms;
		if (job->active) {
			pthread_mutex_unlock(&job->lock);
			return;
		}
		pthread_mutex_unlock(&job->lock);
	} else {
		out->error_code = client->discovery_error;
		out->state = client->discovery_error < 0 ? RUNTIME_MCP_FAILED :
			(client->connected ? (client->capabilities_registered ?
			 RUNTIME_MCP_READY : RUNTIME_MCP_DISCOVERING) : RUNTIME_MCP_DISCONNECTED);
	}
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
	runtime_mcp_status_copy((struct runtime *)runtime,
		runtime->context.mcp.servers[index], out);
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
	runtime_mcp_status_copy((struct runtime *)runtime, client, out);
	return 0;
}

int runtime_mcp_connect(struct runtime *runtime, const char *name)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	int rc;

	if (!client)
		return -ENOENT;
	if (runtime_mcp_busy(runtime, client))
		return -EAGAIN;
	rc = runtime_mcp_client_connect(client, runtime->options.event_cb,
		runtime->options.event_user_data);
	runtime_mcp_update_state(runtime, client,
		rc == 0 ? RUNTIME_MCP_DISCOVERING : RUNTIME_MCP_FAILED, rc);
	return rc;
}

int runtime_mcp_disconnect(struct runtime *runtime, const char *name)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	int rc;

	if (!client)
		return -ENOENT;
	struct runtime_mcp_job *job = runtime_mcp_job_find(runtime, client);
	if (job && runtime_mcp_busy(runtime, client)) {
		pthread_mutex_lock(&job->lock);
		morph_cancel_token_cancel(&job->cancel);
		pthread_mutex_unlock(&job->lock);
		if (job->started) {
			pthread_join(job->thread, NULL);
			job->started = 0;
		}
	}
	rc = runtime_mcp_client_disconnect(client, runtime->options.event_cb,
		runtime->options.event_user_data);
	runtime_mcp_update_state(runtime, client, RUNTIME_MCP_DISCONNECTED, rc);
	return rc;
}

/* One allocation keeps runtime_mcp_list_free() ownership unchanged. */
static int runtime_mcp_copy_list(void **out, int count, const void *items,
	size_t item_size, const size_t *offsets, size_t field_count)
{
	*out = NULL;
	if (count <= 0)
		return 0;
	if ((size_t)count > SIZE_MAX / item_size)
		return -EOVERFLOW;
	size_t bytes = (size_t)count * item_size;

	for (int i = 0; i < count; i++) {
		for (size_t j = 0; j < field_count; j++) {
			const char *value;

			memcpy(&value, (const char *)items + (size_t)i * item_size + offsets[j],
				sizeof(value));
			size_t length = strlen(value) + 1;

			if (length > SIZE_MAX - bytes)
				return -EOVERFLOW;
			bytes += length;
		}
	}
	char *copy = malloc(bytes);

	if (!copy)
		return -ENOMEM;
	memcpy(copy, items, (size_t)count * item_size);
	char *destination = copy + (size_t)count * item_size;

	for (int i = 0; i < count; i++) {
		for (size_t j = 0; j < field_count; j++) {
			const char *value;
			char *field = copy + (size_t)i * item_size + offsets[j];

			memcpy(&value, field, sizeof(value));
			size_t length = strlen(value) + 1;

			memcpy(destination, value, length);
			memcpy(field, &destination, sizeof(destination));
			destination += length;
		}
	}
	*out = copy;
	return 0;
}

int runtime_mcp_list_tools(struct runtime *runtime, const char *name,
			   struct mcp_tool_desc **out, int *count)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	struct mcp_tool_desc *items = NULL;
	struct arena *arena;
	const size_t offsets[] = {
		offsetof(struct mcp_tool_desc, name),
		offsetof(struct mcp_tool_desc, title),
		offsetof(struct mcp_tool_desc, description),
		offsetof(struct mcp_tool_desc, input_schema)
	};
	int rc;
	if (!client || !out || !count)
		return client ? -EINVAL : -ENOENT;
	if (runtime_mcp_busy(runtime, client))
		return -EAGAIN;
	arena = arena_create(0);
	if (!arena)
		return -ENOMEM;
	rc = mcp_ensure_connected(client);
	if (rc == 0)
		rc = mcp_list_tools(client, arena, &items, count);
	if (rc == 0)
		rc = runtime_mcp_copy_list((void **)out, *count, items,
					   sizeof(**out), offsets,
					   sizeof(offsets) / sizeof(offsets[0]));
	arena_destroy(arena);
	return rc;
}

int runtime_mcp_list_resources(struct runtime *runtime, const char *name,
			       struct mcp_resource_desc **out, int *count)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	struct mcp_resource_desc *items = NULL;
	struct arena *arena;
	const size_t offsets[] = {
		offsetof(struct mcp_resource_desc, uri),
		offsetof(struct mcp_resource_desc, name),
		offsetof(struct mcp_resource_desc, description),
		offsetof(struct mcp_resource_desc, mime_type)
	};
	int rc;
	if (!client || !out || !count)
		return client ? -EINVAL : -ENOENT;
	if (runtime_mcp_busy(runtime, client))
		return -EAGAIN;
	arena = arena_create(0);
	if (!arena)
		return -ENOMEM;
	rc = mcp_ensure_connected(client);
	if (rc == 0)
		rc = mcp_list_resources(client, arena, &items, count);
	if (rc == 0)
		rc = runtime_mcp_copy_list((void **)out, *count, items,
					   sizeof(**out), offsets,
					   sizeof(offsets) / sizeof(offsets[0]));
	arena_destroy(arena);
	return rc;
}

int runtime_mcp_list_prompts(struct runtime *runtime, const char *name,
			     struct mcp_prompt_desc **out, int *count)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	struct mcp_prompt_desc *items = NULL;
	struct arena *arena;
	const size_t offsets[] = {
		offsetof(struct mcp_prompt_desc, name),
		offsetof(struct mcp_prompt_desc, description),
		offsetof(struct mcp_prompt_desc, arguments_schema)
	};
	int rc;
	if (!client || !out || !count)
		return client ? -EINVAL : -ENOENT;
	if (runtime_mcp_busy(runtime, client))
		return -EAGAIN;
	arena = arena_create(0);
	if (!arena)
		return -ENOMEM;
	rc = mcp_ensure_connected(client);
	if (rc == 0)
		rc = mcp_list_prompts(client, arena, &items, count);
	if (rc == 0)
		rc = runtime_mcp_copy_list((void **)out, *count, items,
					   sizeof(**out), offsets,
					   sizeof(offsets) / sizeof(offsets[0]));
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
	if (runtime_mcp_busy(runtime, client))
		return -EAGAIN;
	pthread_mutex_lock(&runtime->context.execution_lock);
	pthread_mutex_lock(&runtime->context.tools.mutation_mutex);
	int rc = runtime_mcp_client_discover(
		client, &runtime->context.tools,
		runtime->options.event_cb,
		runtime->options.event_user_data,
		tools ? tools : &t,
		resources ? resources : &r,
		prompts ? prompts : &p);
	pthread_mutex_unlock(&runtime->context.tools.mutation_mutex);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	runtime_mcp_update_state(runtime, client,
		rc == 0 ? RUNTIME_MCP_READY : RUNTIME_MCP_FAILED, rc);
	return rc;
}
