#include "mcp_internal.h"

#include <nb/cancel.h>
#include <nb/cleanup.h>
#include <nb/time.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct mcp_listener {
	struct mcp_client *owner;
	struct mcp_client connection;
	struct mcp_wire_request request;
	nb_thread *thread;
	nb_cancel cancel;
	nb_atomic_int status;
};

static int listener_interrupted(void *opaque)
{
	struct mcp_listener *listener = opaque;
	return nb_cancel_requested(&listener->cancel);
}

static void *listener_run(void *opaque)
{
	struct mcp_listener *listener = opaque;
	struct mcp_wire_reply reply NB_CLEANUP(mcp_wire_reply_cleanup) = {0};
	int rc = mcp_http_connect(&listener->connection);
	if (rc == 0 && listener->owner->session_id && !listener->connection.protocol->per_request) {
		listener->connection.session_id = strdup(listener->owner->session_id);
		if (!listener->connection.session_id)
			rc = NB_ERR_NOMEM;
	}

	if (rc == 0)
		rc = mcp_http_exchange(&listener->connection, &listener->request, 0, &reply);

	if (rc == 0 && reply.http_status >= 400)
		rc = reply.http_status == 405 ? NB_ERR_UNSUPPORTED : NB_ERR_PROTOCOL;

	if (rc == 0 && listener->connection.protocol->per_request) {
		cJSON *response NB_CLEANUP(cJSON_DeleteOwner) = NULL;
		rc = mcp_protocol_decode(listener->owner->config.logger, &reply,
					 listener->request.id, &response);
		const cJSON *result = cJSON_GetObjectItemCaseSensitive(response, "result");
		const cJSON *meta = cJSON_GetObjectItemCaseSensitive(result, "_meta");
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(
			meta, "io.modelcontextprotocol/subscriptionId");
		if (rc == 0 && (!cJSON_IsNumber(id) || id->valuedouble != listener->request.id))
			rc = NB_ERR_PROTOCOL;
	}

	mcp_http_disconnect(&listener->connection);
	listener->status = rc;
	listener->owner->subscription_ended = 1;
	return NULL;
}

void mcp_stop_notifications(struct mcp_client *client)
{
	if (!client)
		return;

	/* The lifecycle owner excludes new calls while stopping this channel. */
	struct mcp_listener *listener = client->listener;
	if (listener) {
		nb_cancel_request(&listener->cancel);
		if (listener->thread)
			(void)nb_thread_join_take(&listener->thread, NULL);

		mcp_wire_request_cleanup(&listener->request);
		free(listener);
		client->listener = NULL;
	}

	if (client->subscription_id > 0 && client->connected && client->protocol &&
	    client->protocol->per_request) {
		cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		if (params &&
		    cJSON_AddNumberToObject(params, "requestId", client->subscription_id)) {
			char *encoded NB_CLEANUP(cJSON_FreeTextOwner) =
				cJSON_PrintUnformatted(params);
			struct mcp_wire_request request NB_CLEANUP(mcp_wire_request_cleanup) = {0};
			struct mcp_wire_reply reply NB_CLEANUP(mcp_wire_reply_cleanup) = {0};
			if (encoded && mcp_protocol_encode(client, client->protocol, -1,
							   "notifications/cancelled", encoded,
							   &request) == 0) {
				if (client->config.transport == MCP_TRANSPORT_STDIO)
					(void)mcp_stdio_exchange(client, &request, 200, &reply);
				else
					(void)mcp_http_exchange(client, &request, 200, &reply);
			}
		}
	}

	client->subscription_id = 0;
	cJSON_DeleteOwner(&client->subscription_filters);
	cJSON_DeleteOwner(&client->subscription_accepted);
	client->subscription_acknowledged = 0;
	client->subscription_ended = 1;
}

static int subscription_filters_valid(const cJSON *filters)
{
	if (!cJSON_IsObject(filters))
		return 0;

	const char *keys[] = {"toolsListChanged", "promptsListChanged", "resourcesListChanged"};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		const cJSON *value = cJSON_GetObjectItemCaseSensitive(filters, keys[i]);
		if (value && !cJSON_IsBool(value))
			return 0;
	}

	const cJSON *uris = cJSON_GetObjectItemCaseSensitive(filters, "resourceSubscriptions");
	if (uris && !cJSON_IsArray(uris))
		return 0;

	const cJSON *uri;
	cJSON_ArrayForEach (uri, uris) {
		if (!cJSON_IsString(uri) || !uri->valuestring[0])
			return 0;
	}

	return 1;
}

static int listener_start_locked(struct mcp_client *client, const cJSON *filters)
{
	if (!client->connected || !client->protocol)
		return NB_ERR_CLOSED;

	if (client->listener || client->subscription_id > 0)
		return NB_ERR_EXISTS;

	if (!client->protocol->per_request && client->config.transport == MCP_TRANSPORT_STDIO)
		return 0;

	if (client->next_req_id >= INT_MAX)
		return NB_ERR_RANGE;

	struct mcp_wire_request request NB_CLEANUP(mcp_wire_request_cleanup) = {0};
	int rc = 0;
	if (client->protocol->per_request) {
		cJSON *params NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateObject();
		cJSON *copied NB_CLEANUP(cJSON_DeleteOwner) = cJSON_Duplicate(filters, 1);
		if (!params || !copied || !cJSON_AddItemToObject(params, "notifications", copied))
			return NB_ERR_NOMEM;

		copied = NULL;
		char *encoded NB_CLEANUP(cJSON_FreeTextOwner) = cJSON_PrintUnformatted(params);
		if (!encoded)
			return NB_ERR_NOMEM;

		int id = client->next_req_id++;
		rc = mcp_protocol_encode(client, client->protocol, id, "subscriptions/listen",
					 encoded, &request);
		if (rc != 0)
			return rc;

		client->subscription_id = id;
	} else {
		request.id = -2;
		request.http_method = "GET";
		const char *header = NULL;
		nb_vec_init(&request.headers, sizeof(header));
		rc = mcp_protocol_add_header_copy(&request.headers, "MCP-Protocol-Version",
						  client->protocol->version);
		if (rc == 0 && client->session_id)
			rc = mcp_protocol_add_header_copy(&request.headers, "Mcp-Session-Id",
							  client->session_id);

		if (rc != 0)
			return rc;
	}

	client->subscription_acknowledged = 0;
	client->subscription_ended = 0;
	client->subscription_filters = cJSON_Duplicate(filters, 1);
	if (!client->subscription_filters)
		return NB_ERR_NOMEM;

	if (client->config.transport == MCP_TRANSPORT_STDIO) {
		struct mcp_wire_reply reply NB_CLEANUP(mcp_wire_reply_cleanup) = {0};
		request.id = -1;
		return mcp_stdio_exchange(client, &request, mcp_request_timeout_ms(client), &reply);
	}

	client->listener = calloc(1, sizeof(*client->listener));
	struct mcp_listener *listener = client->listener;
	if (!listener)
		return NB_ERR_NOMEM;

	listener->owner = client;
	listener->connection.config = client->config;
	listener->connection.protocol = client->protocol;
	listener->connection.handlers = client->handlers;
	listener->connection.notification_target = client;
	listener->connection.shutdown_cancel = client->shutdown_cancel;
	listener->connection.interrupt_check = listener_interrupted;
	listener->connection.interrupt_user_data = listener;
	listener->request = request;
	memset(&request, 0, sizeof(request));
	nb_cancel_init(&listener->cancel);
	rc = nb_thread_create(&listener->thread, listener_run, listener, NULL);
	if (rc != 0)
		mcp_stop_notifications(client);

	return rc;
}

int mcp_listen_notifications(struct mcp_client *client, const cJSON *filters)
{
	if (!client || !subscription_filters_valid(filters))
		return NB_ERR_INVAL;

	nb_mutex_lock(client->lock);
	int rc = listener_start_locked(client, filters);
	int64_t deadline = nb_time_monotonic_ms() + mcp_request_timeout_ms(client);
	while (rc == 0 && client->protocol->per_request &&
	       !nb_atomic_int_load(&client->subscription_acknowledged)) {
		if (nb_atomic_int_load(&client->subscription_ended)) {
			rc = client->listener ? nb_atomic_int_load(&client->listener->status)
					      : NB_ERR_CLOSED;
			if (rc == 0)
				rc = NB_ERR_CLOSED;

			break;
		}

		if (nb_time_monotonic_ms() >= deadline) {
			rc = NB_ERR_TIMEOUT;
			break;
		}

		if (client->config.transport == MCP_TRANSPORT_STDIO) {
			rc = mcp_stdio_poll(client, 100);
			if (rc == NB_ERR_AGAIN || rc == NB_ERR_TIMEOUT)
				rc = 0;
		} else {
			struct http_control control = {.interrupt_check = mcp_client_interrupted,
						       .interrupt_user_data = client};
			rc = http_wait_cancelable(&control, 10);
		}
	}

	if (rc != 0 && rc != NB_ERR_EXISTS)
		mcp_stop_notifications(client);

	nb_mutex_unlock(client->lock);
	return rc;
}

int mcp_listen_catalog_changes(struct mcp_client *client)
{
	if (!client || !client->supports_catalog_notifications)
		return 0;

	int lock_status = nb_mutex_trylock(client->lock);
	if (lock_status != 0)
		return lock_status == NB_ERR_BUSY ? 0 : lock_status;

	int active = client->listener || client->subscription_id > 0;
	if (active && !nb_atomic_int_load(&client->subscription_ended)) {
		nb_mutex_unlock(client->lock);
		return 0;
	}

	cJSON *filters NB_CLEANUP(cJSON_DeleteOwner) =
		client->subscription_filters ? cJSON_Duplicate(client->subscription_filters, 1)
					     : cJSON_CreateObject();
	if (!filters) {
		nb_mutex_unlock(client->lock);
		return NB_ERR_NOMEM;
	}

	if (active)
		mcp_stop_notifications(client);

	nb_mutex_unlock(client->lock);
	const char *keys[] = {"toolsListChanged", "promptsListChanged", "resourcesListChanged"};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		cJSON_DeleteItemFromObjectCaseSensitive(filters, keys[i]);
		if (!cJSON_AddBoolToObject(filters, keys[i], 1))
			return NB_ERR_NOMEM;
	}

	return mcp_listen_notifications(client, filters);
}

int mcp_subscription_resource_set(struct mcp_client *client, const char *uri, int subscribe)
{
	nb_mutex_lock(client->lock);
	cJSON *previous NB_CLEANUP(cJSON_DeleteOwner) =
		client->subscription_filters ? cJSON_Duplicate(client->subscription_filters, 1)
					     : NULL;
	cJSON *filters NB_CLEANUP(cJSON_DeleteOwner) =
		previous ? cJSON_Duplicate(previous, 1) : cJSON_CreateObject();
	if (!filters || (client->subscription_filters && !previous)) {
		nb_mutex_unlock(client->lock);
		return NB_ERR_NOMEM;
	}

	cJSON *uris = cJSON_GetObjectItemCaseSensitive(filters, "resourceSubscriptions");
	if (!uris)
		uris = cJSON_AddArrayToObject(filters, "resourceSubscriptions");

	if (!uris) {
		nb_mutex_unlock(client->lock);
		return NB_ERR_NOMEM;
	}

	int found = 0;
	for (cJSON *value = uris->child; value;) {
		cJSON *next = value->next;
		if (cJSON_IsString(value) && !strcmp(value->valuestring, uri)) {
			found = 1;
			if (!subscribe)
				cJSON_Delete(cJSON_DetachItemViaPointer(uris, value));
		}

		value = next;
	}

	if ((subscribe && found) || (!subscribe && !found)) {
		nb_mutex_unlock(client->lock);
		return 0;
	}

	if (subscribe) {
		cJSON *value NB_CLEANUP(cJSON_DeleteOwner) = cJSON_CreateString(uri);
		if (!value || !cJSON_AddItemToArray(uris, value)) {
			nb_mutex_unlock(client->lock);
			return NB_ERR_NOMEM;
		}

		value = NULL;
	}

	mcp_stop_notifications(client);
	nb_mutex_unlock(client->lock);
	int rc = mcp_listen_notifications(client, filters);
	if (rc == 0 && subscribe) {
		nb_mutex_lock(client->lock);
		const cJSON *accepted = cJSON_GetObjectItemCaseSensitive(
			client->subscription_accepted, "resourceSubscriptions");
		int supported = 0;
		const cJSON *value;
		cJSON_ArrayForEach (value, accepted) {
			if (cJSON_IsString(value) && !strcmp(value->valuestring, uri))
				supported = 1;
		}

		nb_mutex_unlock(client->lock);
		if (!supported)
			rc = NB_ERR_UNSUPPORTED;
	}

	if (rc != 0 && rc != NB_ERR_EXISTS) {
		nb_mutex_lock(client->lock);
		mcp_stop_notifications(client);
		nb_mutex_unlock(client->lock);
		if (previous)
			(void)mcp_listen_notifications(client, previous);
	}

	return rc;
}

int mcp_poll_notifications(struct mcp_client *client, int timeout_ms)
{
	if (!client || timeout_ms < 0)
		return NB_ERR_INVAL;

	if (client->config.transport != MCP_TRANSPORT_STDIO)
		return 0;

	int rc = nb_mutex_trylock(client->lock);
	if (rc != 0)
		return rc == NB_ERR_BUSY ? 0 : rc;

	if (!client->connected) {
		nb_mutex_unlock(client->lock);
		return NB_ERR_CLOSED;
	}

	int64_t deadline = nb_time_monotonic_ms() + (timeout_ms > 0 ? timeout_ms : 10);
	do {
		rc = mcp_stdio_poll(client, timeout_ms);
		timeout_ms = 0;
	} while (rc == NB_ERR_AGAIN && nb_time_monotonic_ms() < deadline);

	nb_mutex_unlock(client->lock);
	return rc == NB_ERR_TIMEOUT || rc == NB_ERR_AGAIN ? 0 : rc;
}
