#include "mcp/mcp.h"
#include "cJSON.h"
#include "util/error.h"
#include "util/log.h"
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
#include "util/array.h"
#include "util/buf.h"

extern char **environ;

char *mcp_build_request(int id, const char *method, const char *params_json);
int mcp_parse_result(const char *resp_json, char **out_result);

#define MCP_STDIO_TIMEOUT_MS 30000

/* ----- Internal: stdio send/recv ----- */

static int mcp_stdio_send_bytes(struct mcp_client *client, const char *json)
{
	size_t len = strlen(json);
	size_t total = 0;
	while (total < len) {
		ssize_t n = write(client->stdin_fd, json + total, len - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			MORPH_RETURN(-errno);
		}
		if (n == 0)
			MORPH_RETURN(-EPIPE);
		total += (size_t)n;
	}
	ssize_t newline;
	do {
		newline = write(client->stdin_fd, "\n", 1);
	} while (newline < 0 && errno == EINTR);
	if (newline < 0)
		MORPH_RETURN(-errno);
	return 0;
}

/* A dead server must report EPIPE without delivering SIGPIPE to the host. */
static int mcp_stdio_send(struct mcp_client *client, const char *json)
{
#ifdef __APPLE__
	if (fcntl(client->stdin_fd, F_SETNOSIGPIPE, 1) < 0)
		MORPH_RETURN(-errno);
	return mcp_stdio_send_bytes(client, json);
#else
	sigset_t blocked, previous, pending;
	int was_pending;
	int rc;

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGPIPE);
	rc = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
	if (rc != 0)
		MORPH_RETURN(-rc);
	sigpending(&pending);
	was_pending = sigismember(&pending, SIGPIPE);
	rc = mcp_stdio_send_bytes(client, json);
	if (rc == -EPIPE && !was_pending) {
		sigpending(&pending);
		if (sigismember(&pending, SIGPIPE)) {
			int signal_number;

			(void)sigwait(&blocked, &signal_number);
		}
	}
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	return rc;
#endif
}

static int64_t mcp_stdio_now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

#define MCP_STDIO_RESPONSE_MAX (16U * 1024U * 1024U)

static int mcp_stdio_recv(struct mcp_client *client, int req_id,
	int timeout_ms, char **response)
{
	morph_buf_t *buffer = &client->read_buffer;
	int64_t deadline = mcp_stdio_now_ms() + timeout_ms;
	char chunk[BUFSIZ];
	int rc;

	*response = NULL;
	if (!buffer->data) {
		rc = morph_buf_init(buffer, BUFSIZ);
		if (rc != 0)
			return rc;
	}
	for (;;) {
		if (client->interrupt_check &&
		    client->interrupt_check(client->interrupt_user_data))
			MORPH_RETURN(-ECANCELED);
		int64_t remaining = deadline - mcp_stdio_now_ms();
		if (remaining <= 0)
			MORPH_RETURN(-ETIMEDOUT);
		char *newline = memchr(buffer->data, '\n', buffer->len);
		if (newline) {
			size_t length = (size_t)(newline - buffer->data);
			char *line = strndup(buffer->data, length);

			if (!line)
				MORPH_RETURN(-ENOMEM);
			memmove(buffer->data, newline + 1, buffer->len - length - 1);
			buffer->len -= length + 1;
			buffer->data[buffer->len] = '\0';
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
			MORPH_RETURN(-errno);
		}
		if (ready == 0)
			continue;
		ssize_t received = read(client->stdout_fd, chunk, sizeof(chunk));

		if (received < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			MORPH_RETURN(-errno);
		}
		if (received == 0)
			MORPH_RETURN(-EPIPE);
		if (buffer->len + (size_t)received > MCP_STDIO_RESPONSE_MAX)
			MORPH_RETURN(-EFBIG);
		rc = morph_buf_append(buffer, chunk, (size_t)received);
		if (rc != 0)
			return rc;
	}
}

/* ----- stdio request: send + recv + parse ----- */

int mcp_stdio_request(struct mcp_client *client, int req_id,
		      const char *method, const char *params_json,
		      char **out_result)
{
	char *req = mcp_build_request(req_id, method, params_json);
	if (!req)
		return -ENOMEM;

	log_info("mcp stdio: sending '%s' to '%s'",
		 method, client->config.name);

	pthread_mutex_lock(&client->lock);
	int rc = mcp_stdio_send(client, req);
	free(req);
	if (rc < 0) {
		pthread_mutex_unlock(&client->lock);
		return rc;
	}

	int timeout_ms = MCP_STDIO_TIMEOUT_MS;
	if (client->config.connect_timeout > 0 &&
	    (client->connecting || client->interrupt_check))
		timeout_ms = client->config.connect_timeout > INT_MAX / 1000 ?
			INT_MAX : client->config.connect_timeout * 1000;
	char *resp = NULL;
	rc = mcp_stdio_recv(client, req_id, timeout_ms, &resp);
	pthread_mutex_unlock(&client->lock);

	if (rc != 0) {
		log_err("mcp stdio: request '%s' failed: %s", method, morph_strerror(rc));
		return rc;
	}

	rc = mcp_parse_result(resp, out_result);
	free(resp);
	return rc;
}

/* ----- Public stdio transport functions ----- */

static int mcp_stdio_spawn(struct mcp_client *client)
{
	if (!client || !client->config.command[0] ||
	    client->config.cmd_args_count < 0 ||
	    client->config.cmd_args_count > MCP_CMD_MAX ||
	    client->config.env_count < 0 || client->config.env_count > MCP_ENV_MAX)
		MORPH_RETURN(-EINVAL);

	int to_child[2], from_child[2];
	if (pipe(to_child) < 0) {
		log_err("mcp stdio: pipe failed: %s", strerror(errno));
		MORPH_RETURN_ERRNO();
	}
	if (pipe(from_child) < 0) {
		int err = errno;
		log_err("mcp stdio: pipe failed: %s", strerror(err));
		close(to_child[0]);
		close(to_child[1]);
		MORPH_RETURN(-err);
	}

	struct arena *arena = arena_create(0);
	morph_array_t environment;
	posix_spawn_file_actions_t actions;
	char *argv[MCP_CMD_MAX + 2];
	pid_t pid = -1;
	int rc;

	if (!arena) {
		close(to_child[0]);
		close(to_child[1]);
		close(from_child[0]);
		close(from_child[1]);
		MORPH_RETURN(-ENOMEM);
	}
	size_t environment_count = 0;
	for (char **item = environ; *item; item++)
		environment_count++;
	rc = morph_array_init_arena(&environment, arena,
		environment_count + (size_t)client->config.env_count + 1, sizeof(*argv));
	if (rc != 0)
		goto out_close_pipes;
	for (char **item = environ; *item; item++) {
		int overridden = 0;

		for (int i = 0; i < client->config.env_count; i++) {
			size_t len = strlen(client->config.env_keys[i]);

			if (strncmp(*item, client->config.env_keys[i], len) == 0 &&
			    (*item)[len] == '=')
				overridden = 1;
		}
		if (!overridden) {
			char **slot = morph_array_push(&environment);

			if (!slot) {
				MORPH_SET_ERR(rc, -ENOMEM);
				goto out_close_pipes;
			}
			*slot = *item;
		}
	}
	for (int i = 0; i < client->config.env_count; i++) {
		morph_buf_t value;
		char **slot;

		rc = morph_buf_init_arena(&value, arena, 64);
		if (rc != 0)
			goto out_close_pipes;
		rc = morph_buf_printf(&value, "%s=%s", client->config.env_keys[i],
			client->config.env_vals[i]);
		if (rc != 0)
			goto out_close_pipes;
		slot = morph_array_push(&environment);
		if (!slot) {
			MORPH_SET_ERR(rc, -ENOMEM);
			goto out_close_pipes;
		}
		*slot = (char *)morph_buf_cstr(&value);
	}
	char **last = morph_array_push(&environment);
	if (!last) {
		MORPH_SET_ERR(rc, -ENOMEM);
		goto out_close_pipes;
	}
	*last = NULL;
	argv[0] = client->config.command;
	for (int i = 0; i < client->config.cmd_args_count; i++)
		argv[i + 1] = client->config.cmd_args[i];
	argv[client->config.cmd_args_count + 1] = NULL;
	if (fcntl(to_child[0], F_SETFD, FD_CLOEXEC) < 0 ||
	    fcntl(to_child[1], F_SETFD, FD_CLOEXEC) < 0 ||
	    fcntl(from_child[0], F_SETFD, FD_CLOEXEC) < 0 ||
	    fcntl(from_child[1], F_SETFD, FD_CLOEXEC) < 0) {
		MORPH_SET_ERR(rc, -errno);
		goto out_close_pipes;
	}
	rc = posix_spawn_file_actions_init(&actions);
	if (rc != 0) {
		MORPH_SET_ERR(rc, -rc);
		goto out_close_pipes;
	}
	rc = posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
	if (rc == 0)
		rc = posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
	if (rc == 0)
		rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
			"/dev/null", O_WRONLY, 0);
	if (rc == 0)
		rc = posix_spawnp(&pid, client->config.command, &actions, NULL,
			argv, environment.elts);
	posix_spawn_file_actions_destroy(&actions);
	if (rc != 0) {
		MORPH_SET_ERR(rc, -rc);
		goto out_close_pipes;
	}
	arena_destroy(arena);
	close(to_child[0]);
	close(from_child[1]);

	client->server_pid = pid;
	client->stdin_fd = to_child[1];
	client->stdout_fd = from_child[0];

	log_info("mcp stdio: started '%s' (pid=%d)", client->config.name, pid);
	return 0;

out_close_pipes:
	arena_destroy(arena);
	close(to_child[0]);
	close(to_child[1]);
	close(from_child[0]);
	close(from_child[1]);
	return rc;
}

/* Serialize pipe setup with spawn so other workers cannot inherit open pipes. */
int mcp_stdio_connect(struct mcp_client *client)
{
	static pthread_mutex_t spawn_lock = PTHREAD_MUTEX_INITIALIZER;
	int rc;

	pthread_mutex_lock(&spawn_lock);
	rc = mcp_stdio_spawn(client);
	pthread_mutex_unlock(&spawn_lock);
	return rc;
}

int mcp_stdio_initialize(struct mcp_client *client)
{
	if (!client)
		return -EINVAL;

	char params_buf[2048];
	snprintf(params_buf, sizeof(params_buf),
		 "{"
		 "\"protocolVersion\":\"%s\","
		 "\"capabilities\":{\"roots\":{\"listChanged\":false}},"
		 "\"clientInfo\":{\"name\":\"morph\",\"version\":\"" MORPH_VERSION "\"}"
		 "}",
		 MCP_PROTOCOL_VERSION);

	char *result = NULL;
	int req_id = client->next_req_id++;
	int rc = mcp_stdio_request(client, req_id, "initialize", params_buf, &result);
	if (rc < 0) {
		log_err("mcp: initialize failed for '%s'", client->config.name);
		return rc;
	}

	cJSON *obj = cJSON_Parse(result);
	if (obj) {
		cJSON *v = cJSON_GetObjectItem(obj, "protocolVersion");
		if (v && cJSON_IsString(v))
			strncpy(client->negotiated_version, v->valuestring,
				sizeof(client->negotiated_version) - 1);

		v = cJSON_GetObjectItem(obj, "serverInfo");
		if (v) {
			cJSON *sn = cJSON_GetObjectItem(v, "name");
			if (sn && cJSON_IsString(sn))
				strncpy(client->server_name, sn->valuestring,
					sizeof(client->server_name) - 1);
			cJSON *sv = cJSON_GetObjectItem(v, "version");
			if (sv && cJSON_IsString(sv))
				strncpy(client->server_version, sv->valuestring,
					sizeof(client->server_version) - 1);
		}

		cJSON *caps = cJSON_GetObjectItem(obj, "capabilities");
		if (caps) {
			client->supports_tools = cJSON_HasObjectItem(caps, "tools");
			client->supports_resources = cJSON_HasObjectItem(caps, "resources");
			client->supports_prompts = cJSON_HasObjectItem(caps, "prompts");
		}

		cJSON_Delete(obj);
	}
	free(result);

	log_info("mcp stdio: initialized '%s' (server=%s v%s, proto=%s)",
		 client->config.name,
		 client->server_name, client->server_version,
		 client->negotiated_version);

	char notif[64];
	snprintf(notif, sizeof(notif),
		 "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

	pthread_mutex_lock(&client->lock);
	mcp_stdio_send(client, notif);
	pthread_mutex_unlock(&client->lock);

	client->connected = 1;
	return 0;
}

void mcp_stdio_disconnect(struct mcp_client *client)
{
	if (!client)
		return;

	if (client->stdin_fd >= 0) {
		close(client->stdin_fd);
		client->stdin_fd = -1;
	}
	if (client->stdout_fd >= 0) {
		close(client->stdout_fd);
		client->stdout_fd = -1;
	}

	if (client->server_pid > 0) {
		kill(client->server_pid, SIGTERM);

		int status;
		int reaped = 0;
		for (int i = 0; i < 10; i++) {
			pid_t ret = waitpid(client->server_pid, &status, WNOHANG);
			if (ret > 0 || (ret < 0 && errno == ECHILD)) {
				reaped = 1;
				break;
			}
			usleep(100000);
		}

		if (!reaped) {
			kill(client->server_pid, SIGKILL);
			while (waitpid(client->server_pid, NULL, 0) < 0 && errno == EINTR)
				;
		}

		log_info("mcp stdio: disconnected '%s' (pid=%d)",
			 client->config.name, client->server_pid);
		client->server_pid = -1;
	}

	morph_buf_cleanup(&client->read_buffer);
	client->connected = 0;
}

int mcp_stdio_ping(struct mcp_client *client)
{
	if (!client)
		return -EINVAL;

	char *result = NULL;
	int req_id = client->next_req_id++;
	int rc = mcp_stdio_request(client, req_id, "ping", NULL, &result);
	free(result);
	return rc;
}
