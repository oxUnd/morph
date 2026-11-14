#define _GNU_SOURCE
#include "mcp/mcp_internal.h"
#include "nb/process.h"
#include "cJSON.h"
#include "errors.h"
#include "nb/cleanup.h"
#include "nb/log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#include <spawn.h>
#include "nb/vec.h"
#include "nb/buf.h"
#include "nb/time.h"

extern char **environ;

/* ----- Internal: stdio send/recv ----- */

static int64_t mcp_stdio_now_ms(void)
{
	int64_t now = nb_time_monotonic_ns();

	return now > 0 ? now / 1000000 : 0;
}

static int mcp_stdio_recv(struct mcp_client *client, int req_id,
	int timeout_ms, char **response)
{
	nb_buf *buffer = &client->read_buffer;
	int64_t deadline = mcp_stdio_now_ms() + timeout_ms;
	char chunk[BUFSIZ];
	int rc;

	if (!buffer->ptr) {
		rc = (nb_buf_init(buffer), nb_buf_reserve(buffer, BUFSIZ) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
	for (;;) {
		if (client->interrupt_check &&
		    client->interrupt_check(client->interrupt_user_data))
			return NB_ERR_CANCELLED;
		int64_t remaining = deadline - mcp_stdio_now_ms();
		if (remaining <= 0)
			return NB_ERR_TIMEOUT;
		nb_str next_line;
		if (nb_buf_next_line(buffer, &next_line)) {
			char *line = strndup(next_line.ptr, next_line.len);
			if (!line)
				return NB_ERR_NOMEM;
			cJSON *object = cJSON_Parse(line);
			cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "id");
			int matches = cJSON_IsNumber(id) && id->valueint == req_id;

			cJSON_Delete(object);
			if (matches) {
				*response = line;
				return 0;
			}
			free(line);
			continue;
		}
		struct pollfd pfd = { .fd = client->stdout_fd, .events = POLLIN };
		int wait_ms = remaining > 100 ? 100 : (int)remaining;
		int ready = poll(&pfd, 1, wait_ms);

		if (ready < 0) {
			if (errno == EINTR)
				continue;
			return nb_status_from_errno(errno, NULL);
		}
		if (ready == 0)
			continue;
		ssize_t received = read(client->stdout_fd, chunk, sizeof(chunk));

		if (received < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			return nb_status_from_errno(errno, NULL);
		}
		if (received == 0)
			return NB_ERR_BROKEN_PIPE;
		if ((size_t)received > MCP_STDIO_MAX_FRAME_BYTES ||
		    nb_buf_len(buffer) > MCP_STDIO_MAX_FRAME_BYTES - (size_t)received) {
			return NB_ERR_TOO_BIG;
		}
		rc = (nb_buf_append(buffer, chunk, (size_t)received) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
	}
}

int mcp_stdio_exchange(struct mcp_client *client, const struct mcp_wire_request *request,
		       int timeout_ms, struct mcp_wire_reply *reply)
{
	if (!client || !request || !request->json || !reply)
		return NB_ERR_INVAL;
	if (reply->json || reply->session_id)
		return NB_ERR_EXISTS;
	struct mcp_wire_reply received NB_CLEANUP(mcp_wire_reply_cleanup) = {0};
	int64_t deadline = mcp_stdio_now_ms() + timeout_ms;
	int rc = nb_fd_write_until(client->stdin_fd, request->json, strlen(request->json),
				   deadline * 1000000, client->interrupt_check,
				   client->interrupt_user_data, NULL);
	if (rc == 0) {
		rc = nb_fd_write_until(client->stdin_fd, "\n", 1, deadline * 1000000,
				       client->interrupt_check, client->interrupt_user_data, NULL);
	}
	if (rc != 0) {
		/* A partial line cannot safely be followed by another request. */
		mcp_stdio_disconnect(client);
		return rc;
	}
	if (request->id < 0) {
		return 0;
	}
	int64_t remaining = deadline - mcp_stdio_now_ms();
	if (remaining <= 0) {
		return NB_ERR_TIMEOUT;
	}
	rc = mcp_stdio_recv(client, request->id, (int)remaining, &received.json);
	if (rc == NB_ERR_TOO_BIG) {
		mcp_stdio_disconnect(client);
	}
	if (rc == 0) {
		*reply = received;
		memset(&received, 0, sizeof(received));
	}
	return rc;
}

/* ----- Public stdio transport functions ----- */

static void mcp_environment_cleanup(nb_vec *environment)
{
	char **entry;
	for (size_t _i_ = 0;
	     _i_ < nb_vec_len(environment) && (entry = (char **)nb_vec_at(environment, _i_)); _i_++)
		free(*entry);
	nb_vec_cleanup(environment);
}

static int mcp_stdio_spawn(struct mcp_client *client)
{
	if (!client || !client->config.command[0] ||
	    client->config.cmd_args_count < 0 ||
	    client->config.cmd_args_count > MCP_CMD_MAX ||
	    client->config.env_count < 0 || client->config.env_count > MCP_ENV_MAX)
		return NB_ERR_INVAL;

	nb_vec environment NB_CLEANUP(mcp_environment_cleanup) = {0};
	char *argv[MCP_CMD_MAX + 2];
	int rc;

	size_t environment_count = 0;
	for (char **item = environ; *item; item++)
		environment_count++;
	rc = (nb_vec_init(&environment, sizeof(*argv)),
	      nb_vec_reserve(&environment, environment_count + (size_t)client->config.env_count + 1)
		      ? 0
		      : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	for (char **item = environ; *item; item++) {
		int overridden = 0;

		for (int i = 0; i < client->config.env_count; i++) {
			size_t len = strlen(client->config.env_keys[i]);

			if (strncmp(*item, client->config.env_keys[i], len) == 0 &&
			    (*item)[len] == '=')
				overridden = 1;
		}
		if (!overridden) {
			char **slot = nb_vec_push(&environment);

			if (!slot) {
				rc = (NB_ERR_NOMEM);
				return rc;
			}
			*slot = strdup(*item);
			if (!*slot) {
				rc = NB_ERR_NOMEM;
				return rc;
			}
		}
	}
	for (int i = 0; i < client->config.env_count; i++) {
		nb_buf value NB_CLEANUP(nb_buf_cleanup) = {0};
		char **slot;

		rc = (nb_buf_init(&value), nb_buf_reserve(&value, 64) ? 0 : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		rc = (nb_buf_printf(&value, "%s=%s", client->config.env_keys[i],
				    client->config.env_vals[i])
			      ? 0
			      : NB_ERR_NOMEM);
		if (rc != 0)
			return rc;
		slot = nb_vec_push(&environment);
		if (!slot) {
			rc = (NB_ERR_NOMEM);
			return rc;
		}
		*slot = (nb_buf_ok(&value) ? strdup(nb_buf_cstr(&value)) : NULL);
		if (!*slot)
			return NB_ERR_NOMEM;
	}
	char **last = nb_vec_push(&environment);
	if (!last) {
		rc = (NB_ERR_NOMEM);
		return rc;
	}
	*last = NULL;
	argv[0] = client->config.command;
	for (int i = 0; i < client->config.cmd_args_count; i++)
		argv[i + 1] = client->config.cmd_args[i];
	argv[client->config.cmd_args_count + 1] = NULL;
	nb_process_options options = {
		.argv = (const char *const *)argv,
		.env = (const char *const *)environment.data,
		.search_path = true,
		.close_fds = true,
		.new_group = true,
		.stderr_mode = NB_PROCESS_NULL,
	};
	rc = nb_process_spawn(&options, &client->child, NULL);
	if (rc != NB_OK)
		return rc;
	client->server_pid = (pid_t)nb_process_id(client->child);
	client->stdin_fd = nb_process_stdin_fd(client->child);
	client->stdout_fd = nb_process_stdout_fd(client->child);

	nb_log_write(client ? client->config.logger : NULL, NB_LOG_INFO,
		     "mcp stdio: started '%s' (pid=%d)", client->config.name, client->server_pid);
	return 0;
}

int mcp_stdio_connect(struct mcp_client *client)
{
	return mcp_stdio_spawn(client);
}

void mcp_stdio_disconnect(struct mcp_client *client)
{
	if (!client)
		return;

	if (client->child) {
		(void)nb_process_terminate(client->child, 1000);
		nb_process_free(client->child);
		client->child = NULL;
		nb_log_write(client->config.logger, NB_LOG_INFO,
			     "mcp stdio: disconnected '%s' (pid=%d)", client->config.name,
			     client->server_pid);
	} else {
		if (client->stdin_fd >= 0)
			close(client->stdin_fd);
		if (client->stdout_fd >= 0)
			close(client->stdout_fd);
	}
	client->stdin_fd = -1;
	client->stdout_fd = -1;
	client->server_pid = -1;
	nb_buf_cleanup(&client->read_buffer);
	client->connected = 0;
}
