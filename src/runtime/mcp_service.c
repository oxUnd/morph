#include "runtime/services.h"
#include "nb/str.h"
#include "nb/utf8.h"
#include "runtime/bootstrap.h"
#include "runtime/mcp.h"
#include "runtime/runtime_internal.h"
#include "nb/cleanup.h"
#include "errors.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static struct mcp_client *runtime_mcp_lookup_client(struct runtime *runtime,
						    const char *name)
{
	return runtime && name ? mcp_registry_borrow(&runtime->context.mcp, name) : NULL;
}

static int runtime_mcp_busy(struct runtime *runtime, struct mcp_client *client)
{
	struct runtime_mcp_job *job = runtime_mcp_job_find(runtime, client);
	int active = 0;

	if (job) {
		nb_mutex_lock(job->lock);
		active = job->active;
		nb_mutex_unlock(job->lock);
		if (!active && job->started && !nb_thread_is_current(job->thread)) {
			nb_thread_join_take(&job->thread, NULL);
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
		nb_mutex_lock(job->lock);
		job->state = state;
		job->error_code = error_code;
		nb_mutex_unlock(job->lock);
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
		nb_mutex_lock(job->lock);
		out->state = job->state;
		out->error_code = job->error_code;
		out->elapsed_ms = job->active ? runtime_mcp_now_ms() - job->started_ms :
			job->elapsed_ms;
		if (job->active) {
			nb_mutex_unlock(job->lock);
			return;
		}
		nb_mutex_unlock(job->lock);
	} else {
		out->error_code = client->discovery_error;
		out->state = client->discovery_error < 0 ? RUNTIME_MCP_FAILED :
			(client->connected ? (client->capabilities_registered ?
			 RUNTIME_MCP_READY : RUNTIME_MCP_DISCOVERING) : RUNTIME_MCP_DISCONNECTED);
	}
	out->connected = client->connected;
	(void)nb_str_copy_to(
		nb_str_from_parts(client->negotiated_version,
				  nb_utf8_clamp_bytes(client->negotiated_version,
						      sizeof(out->negotiated_version) - 1)),
		out->negotiated_version, (sizeof(out->negotiated_version) - 1) + 1);
	(void)nb_str_copy_to(nb_str_from_parts(client->server_name,
					       nb_utf8_clamp_bytes(client->server_name,
								   sizeof(out->server_name) - 1)),
			     out->server_name, (sizeof(out->server_name) - 1) + 1);
	(void)nb_str_copy_to(
		nb_str_from_parts(client->server_version,
				  nb_utf8_clamp_bytes(client->server_version,
						      sizeof(out->server_version) - 1)),
		out->server_version, (sizeof(out->server_version) - 1) + 1);
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
		return NB_ERR_INVAL;
	runtime_mcp_status_copy((struct runtime *)runtime,
		runtime->context.mcp.servers[index], out);
	return 0;
}

int runtime_mcp_find(const struct runtime *runtime, const char *name,
		     struct runtime_mcp_status *out)
{
	struct mcp_client *client;
	if (!runtime || !name || !out)
		return NB_ERR_INVAL;
	client = mcp_registry_borrow((struct mcp_registry *)&runtime->context.mcp, name);
	if (!client)
		return NB_ERR_NOT_FOUND;
	runtime_mcp_status_copy((struct runtime *)runtime, client, out);
	return 0;
}

int runtime_mcp_connect(struct runtime *runtime, const char *name)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	int rc;

	if (!client)
		return NB_ERR_NOT_FOUND;
	if (runtime_mcp_busy(runtime, client))
		return NB_ERR_AGAIN;
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
		return NB_ERR_NOT_FOUND;
	struct runtime_mcp_job *job = runtime_mcp_job_find(runtime, client);
	if (job && runtime_mcp_busy(runtime, client)) {
		nb_mutex_lock(job->lock);
		nb_cancel_request(&job->cancel);
		nb_mutex_unlock(job->lock);
		if (job->started) {
			nb_thread_join_take(&job->thread, NULL);
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
	if (!out || !item_size || (count > 0 && !items))
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	if (count < 0)
		return NB_ERR_RANGE;
	if (count == 0)
		return 0;
	if ((size_t)count > SIZE_MAX / item_size)
		return NB_ERR_RANGE;
	size_t bytes = (size_t)count * item_size;

	for (int i = 0; i < count; i++) {
		for (size_t j = 0; j < field_count; j++) {
			const char *value;

			memcpy(&value, (const char *)items + (size_t)i * item_size + offsets[j],
				sizeof(value));
			size_t length = value ? strlen(value) + 1 : 0;

			if (length > SIZE_MAX - bytes)
				return NB_ERR_RANGE;
			bytes += length;
		}
	}
	char *copy = malloc(bytes);

	if (!copy)
		return NB_ERR_NOMEM;
	memcpy(copy, items, (size_t)count * item_size);
	char *destination = copy + (size_t)count * item_size;

	for (int i = 0; i < count; i++) {
		for (size_t j = 0; j < field_count; j++) {
			const char *value;
			char *field = copy + (size_t)i * item_size + offsets[j];

			memcpy(&value, field, sizeof(value));
			size_t length = value ? strlen(value) + 1 : 0;

			if (value) {
				memcpy(destination, value, length);
				memcpy(field, &destination, sizeof(destination));
				destination += length;
			}
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
	void *copied = NULL;
	const size_t offsets[] = {
		offsetof(struct mcp_tool_desc, name),
		offsetof(struct mcp_tool_desc, title),
		offsetof(struct mcp_tool_desc, description),
		offsetof(struct mcp_tool_desc, input_schema)
	};
	int rc;
	int item_count = 0;
	if (!out || !count)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	*count = 0;
	if (!client)
		return NB_ERR_NOT_FOUND;
	if (runtime_mcp_busy(runtime, client))
		return NB_ERR_AGAIN;
	rc = mcp_ensure_connected(client);
	if (rc == 0)
		rc = mcp_list_tools_copy(client, &items, &item_count);
	if (rc == 0)
		rc = runtime_mcp_copy_list(&copied, item_count, items, sizeof(**out), offsets,
					   sizeof(offsets) / sizeof(offsets[0]));
	mcp_tool_descs_free(items, item_count);
	if (rc == 0) {
		*out = copied;
		*count = item_count;
	}
	return rc;
}

int runtime_mcp_list_resources(struct runtime *runtime, const char *name,
			       struct mcp_resource_desc **out, int *count)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	struct mcp_resource_desc *items = NULL;
	void *copied = NULL;
	const size_t offsets[] = {
		offsetof(struct mcp_resource_desc, uri),
		offsetof(struct mcp_resource_desc, name),
		offsetof(struct mcp_resource_desc, description),
		offsetof(struct mcp_resource_desc, mime_type)
	};
	int rc;
	int item_count = 0;
	if (!out || !count)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	*count = 0;
	if (!client)
		return NB_ERR_NOT_FOUND;
	if (runtime_mcp_busy(runtime, client))
		return NB_ERR_AGAIN;
	rc = mcp_ensure_connected(client);
	if (rc == 0)
		rc = mcp_list_resources_copy(client, &items, &item_count);
	if (rc == 0)
		rc = runtime_mcp_copy_list(&copied, item_count, items, sizeof(**out), offsets,
					   sizeof(offsets) / sizeof(offsets[0]));
	mcp_resource_descs_free(items, item_count);
	if (rc == 0) {
		*out = copied;
		*count = item_count;
	}
	return rc;
}

int runtime_mcp_list_prompts(struct runtime *runtime, const char *name,
			     struct mcp_prompt_desc **out, int *count)
{
	struct mcp_client *client = runtime_mcp_lookup_client(runtime, name);
	struct mcp_prompt_desc *items = NULL;
	void *copied = NULL;
	const size_t offsets[] = {
		offsetof(struct mcp_prompt_desc, name),
		offsetof(struct mcp_prompt_desc, description),
		offsetof(struct mcp_prompt_desc, arguments_schema)
	};
	int rc;
	int item_count = 0;
	if (!out || !count)
		return NB_ERR_INVAL;
	if (*out)
		return NB_ERR_EXISTS;
	*count = 0;
	if (!client)
		return NB_ERR_NOT_FOUND;
	if (runtime_mcp_busy(runtime, client))
		return NB_ERR_AGAIN;
	rc = mcp_ensure_connected(client);
	if (rc == 0)
		rc = mcp_list_prompts_copy(client, &items, &item_count);
	if (rc == 0)
		rc = runtime_mcp_copy_list(&copied, item_count, items, sizeof(**out), offsets,
					   sizeof(offsets) / sizeof(offsets[0]));
	mcp_prompt_descs_free(items, item_count);
	if (rc == 0) {
		*out = copied;
		*count = item_count;
	}
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
		return NB_ERR_NOT_FOUND;
	if (runtime_mcp_busy(runtime, client))
		return NB_ERR_AGAIN;
	nb_mutex_lock(runtime->context.execution_lock);
	nb_mutex_lock(runtime->context.tools.mutation_mutex);
	int rc = runtime_mcp_client_discover(
		client, &runtime->context.tools,
		runtime->options.event_cb,
		runtime->options.event_user_data,
		tools ? tools : &t,
		resources ? resources : &r,
		prompts ? prompts : &p);
	runtime_bootstrap_tool_flags(&runtime->context.tools, &runtime->context.config);
	nb_mutex_unlock(runtime->context.tools.mutation_mutex);
	nb_mutex_unlock(runtime->context.execution_lock);
	runtime_mcp_update_state(runtime, client,
		rc == 0 ? RUNTIME_MCP_READY : RUNTIME_MCP_FAILED, rc);
	return rc;
}
