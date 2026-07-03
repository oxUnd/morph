#include "quickjs.h"
#include "runner_api.h"
#include "cJSON.h"
#include "util/file.h"
#include "util/buf.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RUNNER_MAX_CAPTURE (1024 * 1024)

struct runner_state {
	time_t deadline;
};

int runner_has_cap(const char *cap)
{
	const char *caps;
	size_t cap_len;
	const char *p;

	if (!cap || !*cap)
		return 0;
	caps = getenv("MORPH_DYNAMIC_CAPS");
	if (!caps || !*caps)
		return 0;
	if (strcmp(caps, "*") == 0)
		return 1;
	cap_len = strlen(cap);
	p = caps;
	while (*p) {
		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		if (strncmp(p, cap, cap_len) == 0 &&
		    (p[cap_len] == '\0' || p[cap_len] == ',' ||
		     p[cap_len] == ' ' || p[cap_len] == '\t'))
			return 1;
		while (*p && *p != ',')
			p++;
	}
	return 0;
}

int runner_list_allows(const char *env_name, const char *value)
{
	const char *list;
	size_t value_len;
	const char *p;

	list = getenv(env_name);
	if (!list || !*list)
		return 0;
	if (strcmp(list, "*") == 0)
		return 1;
	value_len = strlen(value ? value : "");
	p = list;
	while (*p) {
		const char *start;
		size_t len;

		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		start = p;
		while (*p && *p != ',')
			p++;
		len = (size_t)(p - start);
		while (len > 0 &&
		       (start[len - 1] == ' ' || start[len - 1] == '\t'))
			len--;
		if (len == 1 && start[0] == '*')
			return 1;
		if (len > 0 && value_len >= len &&
		    strncmp(value, start, len) == 0)
			return 1;
	}
	return 0;
}

JSValue runner_throw_cap(JSContext *ctx, const char *cap)
{
	return JS_ThrowTypeError(ctx, "dynamic tool capability denied: %s",
				 cap ? cap : "unknown");
}

static JSValue js_fs_read_text(JSContext *ctx, JSValueConst this_val,
			       int argc, JSValueConst *argv)
{
	const char *path;
	char *data;
	size_t len;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "readText(path) requires path");
	if (!runner_has_cap("fs_read"))
		return runner_throw_cap(ctx, "fs_read");
	path = JS_ToCString(ctx, argv[0]);
	if (!path)
		return JS_EXCEPTION;
	if (!runner_list_allows("MORPH_DYNAMIC_ALLOWED_READ", path)) {
		JS_FreeCString(ctx, path);
		return JS_ThrowTypeError(ctx, "read path denied");
	}
	data = file_read_all(path, &len);
	JS_FreeCString(ctx, path);
	if (!data)
		return JS_ThrowInternalError(ctx, "failed to read file");
	JSValue out = JS_NewStringLen(ctx, data, len);
	free(data);
	return out;
}

static JSValue js_fs_write_text(JSContext *ctx, JSValueConst this_val,
				int argc, JSValueConst *argv)
{
	const char *path;
	const char *text;
	size_t len;
	int rc;

	(void)this_val;
	if (argc < 2)
		return JS_ThrowTypeError(ctx,
					 "writeText(path, text) requires two args");
	if (!runner_has_cap("fs_write"))
		return runner_throw_cap(ctx, "fs_write");
	path = JS_ToCString(ctx, argv[0]);
	if (!path)
		return JS_EXCEPTION;
	if (!runner_list_allows("MORPH_DYNAMIC_ALLOWED_WRITE", path)) {
		JS_FreeCString(ctx, path);
		return JS_ThrowTypeError(ctx, "write path denied");
	}
	text = JS_ToCStringLen(ctx, &len, argv[1]);
	if (!text) {
		JS_FreeCString(ctx, path);
		return JS_EXCEPTION;
	}
	rc = file_write_all(path, text, len);
	JS_FreeCString(ctx, text);
	JS_FreeCString(ctx, path);
	if (rc < 0)
		return JS_ThrowInternalError(ctx, "failed to write file");
	return JS_NewBool(ctx, 1);
}

static char *capture_command(const char *cmd)
{
	FILE *fp;
	morph_buf_t buf;
	char tmp[BUFSIZ];
	int rc;

	fp = popen(cmd, "r");
	if (!fp)
		return NULL;
	rc = morph_buf_init(&buf, BUFSIZ);
	if (rc != 0) {
		pclose(fp);
		return NULL;
	}
	while (fgets(tmp, sizeof(tmp), fp)) {
		size_t n = strlen(tmp);
		if (buf.len + n > RUNNER_MAX_CAPTURE)
			n = RUNNER_MAX_CAPTURE - buf.len;
		if (n > 0 && morph_buf_append(&buf, tmp, n) != 0)
			break;
		if (buf.len >= RUNNER_MAX_CAPTURE)
			break;
	}
	(void)pclose(fp);
	return morph_buf_detach(&buf);
}

static JSValue js_exec(JSContext *ctx, JSValueConst this_val,
		       int argc, JSValueConst *argv)
{
	const char *cmd;
	char *out;
	JSValue value;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "exec(command) requires command");
	if (!runner_has_cap("shell") && !runner_has_cap("process"))
		return runner_throw_cap(ctx, "shell");
	cmd = JS_ToCString(ctx, argv[0]);
	if (!cmd)
		return JS_EXCEPTION;
	if (!runner_list_allows("MORPH_DYNAMIC_ALLOWED_COMMANDS", cmd)) {
		JS_FreeCString(ctx, cmd);
		return JS_ThrowTypeError(ctx, "command denied");
	}
	out = capture_command(cmd);
	JS_FreeCString(ctx, cmd);
	if (!out)
		return JS_ThrowInternalError(ctx, "command failed");
	value = JS_NewString(ctx, out);
	free(out);
	return value;
}

static char *shell_quote(const char *s)
{
	morph_buf_t buf;
	int rc;

	rc = morph_buf_init(&buf, 128);
	if (rc != 0)
		return NULL;
	if (morph_buf_putc(&buf, '\'') != 0)
		goto fail;
	for (const char *p = s ? s : ""; *p; p++) {
		if (*p == '\'')
			rc = morph_buf_puts(&buf, "'\\''");
		else
			rc = morph_buf_putc(&buf, *p);
		if (rc != 0)
			goto fail;
	}
	if (morph_buf_putc(&buf, '\'') != 0)
		goto fail;
	return morph_buf_detach(&buf);
fail:
	morph_buf_cleanup(&buf);
	return NULL;
}

static JSValue make_fetch_response(JSContext *ctx, const char *url,
				   const char *body);

static JSValue js_fetch(JSContext *ctx, JSValueConst this_val,
			int argc, JSValueConst *argv)
{
	const char *url;
	char *quoted;
	char *cmd;
	char *out;
	JSValue value;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "fetch(url) requires url");
	if (!runner_has_cap("network"))
		return runner_throw_cap(ctx, "network");
	url = JS_ToCString(ctx, argv[0]);
	if (!url)
		return JS_EXCEPTION;
	if (!runner_list_allows("MORPH_DYNAMIC_ALLOWED_NETWORK", url)) {
		JS_FreeCString(ctx, url);
		return JS_ThrowTypeError(ctx, "network target denied");
	}
	quoted = shell_quote(url);
	if (!quoted)
		return JS_ThrowOutOfMemory(ctx);
	size_t cmd_len = strlen(quoted) + 64;
	cmd = malloc(cmd_len);
	if (!cmd) {
		JS_FreeCString(ctx, url);
		free(quoted);
		return JS_ThrowOutOfMemory(ctx);
	}
	snprintf(cmd, cmd_len, "curl -LfsS --max-time 30 %s", quoted);
	free(quoted);
	out = capture_command(cmd);
	free(cmd);
	if (!out) {
		JS_FreeCString(ctx, url);
		return JS_ThrowInternalError(ctx, "fetch failed");
	}
	value = make_fetch_response(ctx, url, out);
	JS_FreeCString(ctx, url);
	free(out);
	return value;
}

static JSValue js_fetch_text(JSContext *ctx, JSValueConst this_val,
			     int argc, JSValueConst *argv)
{
	JSValue body;

	(void)argc;
	(void)argv;
	body = JS_GetPropertyStr(ctx, this_val, "_body");
	if (JS_IsException(body))
		return body;
	return body;
}

static JSValue js_fetch_to_string(JSContext *ctx, JSValueConst this_val,
				  int argc, JSValueConst *argv)
{
	return js_fetch_text(ctx, this_val, argc, argv);
}

static JSValue js_fetch_index_of(JSContext *ctx, JSValueConst this_val,
				 int argc, JSValueConst *argv)
{
	JSValue body_val;
	const char *body;
	const char *needle;
	const char *found;
	int pos = -1;

	if (argc < 1)
		return JS_NewInt32(ctx, -1);
	body_val = JS_GetPropertyStr(ctx, this_val, "_body");
	body = JS_ToCString(ctx, body_val);
	needle = JS_ToCString(ctx, argv[0]);
	if (body && needle) {
		found = strstr(body, needle);
		if (found)
			pos = (int)(found - body);
	}
	if (needle)
		JS_FreeCString(ctx, needle);
	if (body)
		JS_FreeCString(ctx, body);
	JS_FreeValue(ctx, body_val);
	return JS_NewInt32(ctx, pos);
}

static JSValue js_fetch_slice(JSContext *ctx, JSValueConst this_val,
			      int argc, JSValueConst *argv)
{
	JSValue body_val;
	const char *body;
	size_t len;
	int32_t start = 0;
	int32_t end;
	int32_t span;
	JSValue out;

	body_val = JS_GetPropertyStr(ctx, this_val, "_body");
	body = JS_ToCStringLen(ctx, &len, body_val);
	if (!body) {
		JS_FreeValue(ctx, body_val);
		return JS_EXCEPTION;
	}
	end = (int32_t)len;
	if (argc > 0)
		(void)JS_ToInt32(ctx, &start, argv[0]);
	if (argc > 1)
		(void)JS_ToInt32(ctx, &end, argv[1]);
	if (start < 0)
		start = (int32_t)len + start;
	if (end < 0)
		end = (int32_t)len + end;
	if (start < 0)
		start = 0;
	if (end < start)
		end = start;
	if ((size_t)start > len)
		start = (int32_t)len;
	if ((size_t)end > len)
		end = (int32_t)len;
	span = end - start;
	out = JS_NewStringLen(ctx, body + start, (size_t)span);
	JS_FreeCString(ctx, body);
	JS_FreeValue(ctx, body_val);
	return out;
}

static JSValue make_fetch_response(JSContext *ctx, const char *url,
				   const char *body)
{
	JSValue obj;
	size_t len;

	len = strlen(body ? body : "");
	obj = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, 1));
	JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, 200));
	JS_SetPropertyStr(ctx, obj, "url", JS_NewString(ctx, url ? url : ""));
	JS_SetPropertyStr(ctx, obj, "body", JS_NewStringLen(ctx, body, len));
	JS_SetPropertyStr(ctx, obj, "content", JS_NewStringLen(ctx, body, len));
	JS_SetPropertyStr(ctx, obj, "length", JS_NewInt64(ctx, (int64_t)len));
	JS_SetPropertyStr(ctx, obj, "_body", JS_NewStringLen(ctx, body, len));
	JS_SetPropertyStr(ctx, obj, "text",
			  JS_NewCFunction(ctx, js_fetch_text, "text", 0));
	JS_SetPropertyStr(ctx, obj, "toString",
			  JS_NewCFunction(ctx, js_fetch_to_string,
					  "toString", 0));
	JS_SetPropertyStr(ctx, obj, "valueOf",
			  JS_NewCFunction(ctx, js_fetch_to_string,
					  "valueOf", 0));
	JS_SetPropertyStr(ctx, obj, "slice",
			  JS_NewCFunction(ctx, js_fetch_slice, "slice", 2));
	JS_SetPropertyStr(ctx, obj, "substring",
			  JS_NewCFunction(ctx, js_fetch_slice,
					  "substring", 2));
	JS_SetPropertyStr(ctx, obj, "indexOf",
			  JS_NewCFunction(ctx, js_fetch_index_of,
					  "indexOf", 1));
	return obj;
}

static int resolve_promise(JSContext *ctx, JSValue *value)
{
	JSValue then;
	JSValue result;
	JSRuntime *rt;
	JSPromiseStateEnum state;
	int is_promise;

	then = JS_GetPropertyStr(ctx, *value, "then");
	is_promise = JS_IsFunction(ctx, then);
	JS_FreeValue(ctx, then);
	if (!is_promise)
		return 0;

	rt = JS_GetRuntime(ctx);
	while (JS_PromiseState(ctx, *value) == JS_PROMISE_PENDING) {
		JSContext *job_ctx = NULL;
		int rc = JS_ExecutePendingJob(rt, &job_ctx);
		if (rc < 0)
			return -EINVAL;
		if (rc == 0)
			break;
	}
	state = JS_PromiseState(ctx, *value);
	if (state == JS_PROMISE_PENDING) {
		JS_ThrowInternalError(ctx, "promise did not settle");
		return -EINVAL;
	}
	result = JS_PromiseResult(ctx, *value);
	JS_FreeValue(ctx, *value);
	if (state == JS_PROMISE_REJECTED) {
		*value = JS_UNDEFINED;
		(void)JS_Throw(ctx, result);
		return -EINVAL;
	}
	*value = result;
	return 0;
}

static JSValue js_env_get(JSContext *ctx, JSValueConst this_val,
			  int argc, JSValueConst *argv)
{
	const char *name;
	const char *value;
	JSValue out;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "env.get(name) requires name");
	if (!runner_has_cap("env"))
		return runner_throw_cap(ctx, "env");
	name = JS_ToCString(ctx, argv[0]);
	if (!name)
		return JS_EXCEPTION;
	value = getenv(name);
	out = value ? JS_NewString(ctx, value) : JS_NULL;
	JS_FreeCString(ctx, name);
	return out;
}

static JSValue js_tool_call(JSContext *ctx, JSValueConst this_val,
			    int argc, JSValueConst *argv)
{
	(void)this_val;
	(void)argc;
	(void)argv;
	if (!runner_has_cap("mcp") && !runner_has_cap("model"))
		return runner_throw_cap(ctx, "mcp");
	return JS_ThrowTypeError(ctx, "morph.tool.call is not available yet");
}

static void install_morph_api(JSContext *ctx)
{
	JSValue global = JS_GetGlobalObject(ctx);
	JSValue morph = JS_NewObject(ctx);
	JSValue fs = JS_NewObject(ctx);
	JSValue env = JS_NewObject(ctx);
	JSValue tool = JS_NewObject(ctx);

	JS_SetPropertyStr(ctx, fs, "readText",
			  JS_NewCFunction(ctx, js_fs_read_text,
					  "readText", 1));
	JS_SetPropertyStr(ctx, fs, "writeText",
			  JS_NewCFunction(ctx, js_fs_write_text,
					  "writeText", 2));
	JS_SetPropertyStr(ctx, env, "get",
			  JS_NewCFunction(ctx, js_env_get, "get", 1));
	JS_SetPropertyStr(ctx, tool, "call",
			  JS_NewCFunction(ctx, js_tool_call, "call", 2));
	JS_SetPropertyStr(ctx, morph, "fs", fs);
	JS_SetPropertyStr(ctx, morph, "env", env);
	JS_SetPropertyStr(ctx, morph, "tool", tool);
	JS_SetPropertyStr(ctx, morph, "exec",
			  JS_NewCFunction(ctx, js_exec, "exec", 1));
	JS_SetPropertyStr(ctx, morph, "fetch",
			  JS_NewCFunction(ctx, js_fetch, "fetch", 1));
	JS_SetPropertyStr(ctx, global, "morph", morph);
	install_media_api(ctx);
	JS_FreeValue(ctx, global);
}

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
	struct runner_state *state = opaque;

	(void)rt;
	if (!state || state->deadline <= 0)
		return 0;
	return time(NULL) >= state->deadline;
}

static void print_json_string(FILE *out, const char *s)
{
	fputc('"', out);
	for (const unsigned char *p = (const unsigned char *)(s ? s : "");
	     *p; p++) {
		switch (*p) {
		case '\\':
			fputs("\\\\", out);
			break;
		case '"':
			fputs("\\\"", out);
			break;
		case '\n':
			fputs("\\n", out);
			break;
		case '\r':
			fputs("\\r", out);
			break;
		case '\t':
			fputs("\\t", out);
			break;
		default:
			if (*p < 0x20)
				fprintf(out, "\\u%04x", *p);
			else
				fputc((int)*p, out);
			break;
		}
	}
	fputc('"', out);
}

static void print_error_response(int id, const char *message)
{
	fputs("{\"jsonrpc\":\"2.0\",\"id\":", stdout);
	fprintf(stdout, "%d", id);
	fputs(",\"error\":{\"code\":-32000,\"message\":", stdout);
	print_json_string(stdout, message ? message : "runtime error");
	fputs("}}\n", stdout);
}

static void print_exception(JSContext *ctx, int id)
{
	JSValue exception = JS_GetException(ctx);
	const char *msg = JS_ToCString(ctx, exception);

	print_error_response(id, msg ? msg : "JavaScript exception");
	if (msg)
		JS_FreeCString(ctx, msg);
	JS_FreeValue(ctx, exception);
}

static int check_script(JSContext *ctx, const char *path)
{
	char *source;
	size_t len;
	JSValue val;

	source = file_read_all(path, &len);
	if (!source)
		return -ENOENT;
	val = JS_Eval(ctx, source, len, path, JS_EVAL_TYPE_GLOBAL);
	free(source);
	if (JS_IsException(val)) {
		print_exception(ctx, 1);
		return -EINVAL;
	}
	JS_FreeValue(ctx, val);
	return 0;
}

static int run_script(JSContext *ctx, const char *path)
{
	char *source = NULL;
	char *line = NULL;
	char *params_json = NULL;
	size_t source_len;
	size_t line_cap = 0;
	int id = 1;
	int rc = 0;
	cJSON *root = NULL;
	cJSON *params;
	JSValue val = JS_UNDEFINED;
	JSValue global = JS_UNDEFINED;
	JSValue fn = JS_UNDEFINED;
	JSValue args = JS_UNDEFINED;
	JSValue ret = JS_UNDEFINED;
	JSValue json = JS_UNDEFINED;
	const char *json_str;

	source = file_read_all(path, &source_len);
	if (!source)
		return -ENOENT;
	val = JS_Eval(ctx, source, source_len, path, JS_EVAL_TYPE_GLOBAL);
	free(source);
	source = NULL;
	if (JS_IsException(val)) {
		print_exception(ctx, id);
		return -EINVAL;
	}
	JS_FreeValue(ctx, val);
	val = JS_UNDEFINED;

	if (getline(&line, &line_cap, stdin) < 0) {
		print_error_response(id, "missing JSON-RPC request");
		rc = -EINVAL;
		goto out;
	}
	root = cJSON_Parse(line);
	if (!root) {
		print_error_response(id, "invalid JSON-RPC request");
		rc = -EINVAL;
		goto out;
	}
	{
		cJSON *id_item = cJSON_GetObjectItem(root, "id");
		if (cJSON_IsNumber(id_item))
			id = id_item->valueint;
	}
	params = cJSON_GetObjectItem(root, "params");
	params_json = params ? cJSON_PrintUnformatted(params) : strdup("{}");
	if (!params_json) {
		rc = -ENOMEM;
		goto out;
	}
	args = JS_ParseJSON(ctx, params_json, strlen(params_json), "<params>");
	if (JS_IsException(args)) {
		print_exception(ctx, id);
		rc = -EINVAL;
		goto out;
	}

	global = JS_GetGlobalObject(ctx);
	fn = JS_GetPropertyStr(ctx, global, "run");
	if (!JS_IsFunction(ctx, fn)) {
		print_error_response(id, "script must define global run(args)");
		rc = -EINVAL;
		goto out;
	}
	ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &args);
	if (JS_IsException(ret)) {
		print_exception(ctx, id);
		rc = -EINVAL;
		goto out;
	}
	if (resolve_promise(ctx, &ret) < 0) {
		print_exception(ctx, id);
		rc = -EINVAL;
		goto out;
	}
	json = JS_JSONStringify(ctx, ret, JS_UNDEFINED, JS_UNDEFINED);
	if (JS_IsException(json)) {
		print_exception(ctx, id);
		rc = -EINVAL;
		goto out;
	}
	if (JS_IsUndefined(json))
		json_str = "null";
	else
		json_str = JS_ToCString(ctx, json);
	if (!json_str) {
		print_exception(ctx, id);
		rc = -EINVAL;
		goto out;
	}
	fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}\n",
		id, json_str);
	if (!JS_IsUndefined(json))
		JS_FreeCString(ctx, json_str);

out:
	JS_FreeValue(ctx, json);
	JS_FreeValue(ctx, ret);
	JS_FreeValue(ctx, args);
	JS_FreeValue(ctx, fn);
	JS_FreeValue(ctx, global);
	cJSON_Delete(root);
	free(params_json);
	free(line);
	return rc;
}

int main(int argc, char **argv)
{
	JSRuntime *rt;
	JSContext *ctx;
	struct runner_state state;
	int timeout;
	int check_only = 0;
	const char *script;
	int rc;

	if (argc < 2) {
		fprintf(stderr, "usage: morph-js-runner [--check] <script.js>\n");
		return 2;
	}
	if (strcmp(argv[1], "--check") == 0) {
		check_only = 1;
		if (argc < 3) {
			fprintf(stderr, "usage: morph-js-runner --check <script.js>\n");
			return 2;
		}
		script = argv[2];
	} else {
		script = argv[1];
	}
	timeout = 30;
	if (getenv("MORPH_DYNAMIC_TIMEOUT"))
		timeout = atoi(getenv("MORPH_DYNAMIC_TIMEOUT"));
	if (js_media_init() < 0) {
		fprintf(stderr, "failed to initialize media bindings\n");
		return 1;
	}
	memset(&state, 0, sizeof(state));
	if (timeout > 0)
		state.deadline = time(NULL) + timeout;
	rt = JS_NewRuntime();
	if (!rt) {
		fprintf(stderr, "failed to create QuickJS runtime\n");
		js_media_shutdown();
		return 1;
	}
	JS_SetInterruptHandler(rt, interrupt_handler, &state);
	ctx = JS_NewContext(rt);
	if (!ctx) {
		JS_FreeRuntime(rt);
		fprintf(stderr, "failed to create QuickJS context\n");
		js_media_shutdown();
		return 1;
	}
	install_morph_api(ctx);
	rc = check_only ? check_script(ctx, script) : run_script(ctx, script);
	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);
	js_media_shutdown();
	return rc == 0 ? 0 : 1;
}
