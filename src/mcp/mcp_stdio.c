#include "mcp/mcp.h"
#include "cJSON.h"
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

char *mcp_build_request(int id, const char *method, const char *params_json);
int mcp_parse_result(const char *resp_json, char **out_result);

#define MCP_STDIO_TIMEOUT_MS 30000

/* ----- Internal: stdio send/recv ----- */

static int mcp_stdio_send(struct mcp_client *client, const char *json)
{
	size_t len = strlen(json);
	size_t total = 0;
	while (total < len) {
		ssize_t n = write(client->stdin_fd, json + total, len - total);
		if (n < 0) {
			log_err("mcp stdio: write failed: %s", strerror(errno));
			return -EIO;
		}
		total += (size_t)n;
	}
	if (write(client->stdin_fd, "\n", 1) < 0) {
		log_err("mcp stdio: newline write failed: %s", strerror(errno));
		return -EIO;
	}
	return 0;
}

static char *mcp_stdio_recv(struct mcp_client *client, int timeout_ms)
{
	char buf[MCP_READ_BUF_MAX];
	size_t pos = 0;
	int skipped = 0;

	for (;;) {
		struct pollfd pfd;
		pfd.fd = client->stdout_fd;
		pfd.events = POLLIN;

		int ret = poll(&pfd, 1, timeout_ms);
		if (ret < 0) {
			log_err("mcp stdio: poll failed: %s", strerror(errno));
			return NULL;
		}
		if (ret == 0) {
			log_err("mcp stdio: read timeout after %d ms", timeout_ms);
			return NULL;
		}

		ssize_t n = read(client->stdout_fd, buf + pos,
				 sizeof(buf) - pos - 1);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			log_err("mcp stdio: read failed: %s", strerror(errno));
			return NULL;
		}
		if (n == 0) {
			log_err("mcp stdio: server process exited");
			return NULL;
		}

		pos += (size_t)n;
		buf[pos] = '\0';

		char *nl = strchr(buf, '\n');
		if (!nl) {
			if (pos >= sizeof(buf) - 1) {
				log_warn("mcp stdio: line too long, flushing");
				pos = 0;
			}
			continue;
		}

		*nl = '\0';
		size_t line_len = (size_t)(nl - buf);

		if (line_len > 0 && buf[line_len - 1] == '\r')
			buf[--line_len] = '\0';

		if (line_len == 0 || buf[0] != '{') {
			if (!skipped)
				log_warn("mcp stdio: skipping non-JSON: %.80s", buf);
			skipped++;
			size_t remain = pos - line_len - 1;
			memmove(buf, nl + 1, remain);
			pos = remain;
			continue;
		}

		char *out = malloc(line_len + 1);
		if (out)
			memcpy(out, buf, line_len + 1);
		return out;
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

	char *resp = mcp_stdio_recv(client, MCP_STDIO_TIMEOUT_MS);
	pthread_mutex_unlock(&client->lock);

	if (!resp) {
		log_err("mcp stdio: no response for '%s'", method);
		return -ETIMEDOUT;
	}

	rc = mcp_parse_result(resp, out_result);
	free(resp);
	return rc;
}

/* ----- Public stdio transport functions ----- */

int mcp_stdio_connect(struct mcp_client *client)
{
	if (!client || !client->config.command[0])
		return -EINVAL;

	int to_child[2], from_child[2];
	if (pipe(to_child) < 0 || pipe(from_child) < 0) {
		log_err("mcp stdio: pipe failed: %s", strerror(errno));
		return -errno;
	}

	pid_t pid = fork();
	if (pid < 0) {
		log_err("mcp stdio: fork failed: %s", strerror(errno));
		close(to_child[0]); close(to_child[1]);
		close(from_child[0]); close(from_child[1]);
		return -errno;
	}

	if (pid == 0) {
		close(to_child[1]);
		close(from_child[0]);

		dup2(to_child[0], STDIN_FILENO);
		dup2(from_child[1], STDOUT_FILENO);

		close(to_child[0]);
		close(from_child[1]);

		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}

		int argc = 1 + client->config.cmd_args_count;
		char **argv = calloc((size_t)(argc + 1), sizeof(char *));
		if (!argv)
			_exit(127);

		argv[0] = client->config.command;
		for (int i = 0; i < client->config.cmd_args_count; i++)
			argv[i + 1] = client->config.cmd_args[i];
		argv[argc] = NULL;

		for (int i = 0; i < client->config.env_count; i++) {
			char envbuf[64 + MCP_ENV_VAL_MAX];
			snprintf(envbuf, sizeof(envbuf), "%s=%s",
				 client->config.env_keys[i],
				 client->config.env_vals[i]);
			putenv(envbuf);
		}

		execvp(client->config.command, argv);
		_exit(127);
	}

	close(to_child[0]);
	close(from_child[1]);

	client->server_pid = pid;
	client->stdin_fd = to_child[1];
	client->stdout_fd = from_child[0];

	log_info("mcp stdio: started '%s' (pid=%d)", client->config.name, pid);
	return 0;
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
		 "\"clientInfo\":{\"name\":\"morph\",\"version\":\"0.1.0\"}"
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
		for (int i = 0; i < 10; i++) {
			pid_t ret = waitpid(client->server_pid, &status, WNOHANG);
			if (ret > 0 || (ret < 0 && errno == ECHILD))
				break;
			usleep(100000);
		}

		kill(client->server_pid, SIGKILL);
		waitpid(client->server_pid, NULL, 0);

		log_info("mcp stdio: disconnected '%s' (pid=%d)",
			 client->config.name, client->server_pid);
		client->server_pid = -1;
	}

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
