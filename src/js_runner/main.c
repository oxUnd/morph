#include "quickjs.h"
#include "runner_api.h"
#include "js_process.h"
#include "js_fetch.h"
#include "cJSON.h"
#include "util/file.h"
#include "util/buf.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
		if (strncmp(p, cap, cap_len) == 0 && (p[cap_len] == '\0' || p[cap_len] == ',' ||
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
		while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
			len--;
		if (len == 1 && start[0] == '*')
			return 1;
		if (len > 0 && value_len >= len && strncmp(value, start, len) == 0) {
			int filesystem = strcmp(env_name, "MORPH_DYNAMIC_ALLOWED_READ") == 0 ||
					 strcmp(env_name, "MORPH_DYNAMIC_ALLOWED_WRITE") == 0;

			if (!filesystem || value_len == len || start[len - 1] == '/' ||
			    value[len] == '/')
				return 1;
		}
	}
	return 0;
}

JSValue runner_throw_cap(JSContext *ctx, const char *cap)
{
	return JS_ThrowTypeError(ctx, "dynamic tool capability denied: %s", cap ? cap : "unknown");
}

char *runner_authorize_path_copy(JSContext *ctx, const char *path, int write)
{
	const char *cap = write ? "fs_write" : "fs_read";
	const char *allow_env =
		write ? "MORPH_DYNAMIC_ALLOWED_WRITE" : "MORPH_DYNAMIC_ALLOWED_READ";
	const char *root_env = write ? "MORPH_DYNAMIC_OUTPUT_DIR" : "MORPH_DYNAMIC_WORKDIR";
	const char *root;
	char *candidate;
	char *resolved;

	if (!runner_has_cap(cap)) {
		runner_throw_cap(ctx, cap);
		return NULL;
	}
	if (!path || !*path) {
		JS_ThrowTypeError(ctx, "%s path is empty", write ? "write" : "read");
		return NULL;
	}
	if (file_path_is_absolute(path)) {
		candidate = strdup(path);
	} else {
		root = getenv(root_env);
		if (!root || !*root) {
			JS_ThrowInternalError(ctx, "dynamic tool %s directory is unavailable",
					      write ? "output" : "work");
			return NULL;
		}
		candidate = file_path_full_alloc(root, path);
	}
	if (!candidate) {
		JS_ThrowOutOfMemory(ctx);
		return NULL;
	}
	resolved = file_resolve_path_copy(candidate);
	free(candidate);
	if (!resolved) {
		JS_ThrowInternalError(ctx, "failed to resolve %s path", write ? "write" : "read");
		return NULL;
	}
	if (!runner_list_allows(allow_env, resolved)) {
		free(resolved);
		JS_ThrowTypeError(ctx, "%s path denied", write ? "write" : "read");
		return NULL;
	}
	return resolved;
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

static JSValue js_env_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
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

static JSValue js_tool_call(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
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
	JSValue env = JS_NewObject(ctx);
	JSValue tool = JS_NewObject(ctx);

	JS_SetPropertyStr(ctx, env, "get", JS_NewCFunction(ctx, js_env_get, "get", 1));
	JS_SetPropertyStr(ctx, tool, "call", JS_NewCFunction(ctx, js_tool_call, "call", 2));
	JS_SetPropertyStr(ctx, morph, "env", env);
	JS_SetPropertyStr(ctx, morph, "tool", tool);
	js_process_install(ctx, morph);
	js_fetch_install(ctx, morph);
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
	for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
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
	JSValue stack = JS_UNDEFINED;
	const char *msg = JS_ToCString(ctx, exception);
	const char *stack_str = NULL;
	morph_buf_t buf;

	stack = JS_GetPropertyStr(ctx, exception, "stack");
	if (!JS_IsUndefined(stack) && !JS_IsNull(stack))
		stack_str = JS_ToCString(ctx, stack);
	if (morph_buf_init(&buf, 512) == 0) {
		if (morph_buf_puts(&buf, msg ? msg : "JavaScript exception") == 0 && stack_str &&
		    *stack_str && morph_buf_puts(&buf, "\nStack:\n") == 0 &&
		    morph_buf_puts(&buf, stack_str) == 0)
			print_error_response(id, morph_buf_cstr_borrow(&buf));
		else
			print_error_response(id, msg ? msg : "JavaScript exception");
		morph_buf_cleanup(&buf);
	} else {
		print_error_response(id, msg ? msg : "JavaScript exception");
	}
	if (stack_str)
		JS_FreeCString(ctx, stack_str);
	if (msg)
		JS_FreeCString(ctx, msg);
	JS_FreeValue(ctx, stack);
	JS_FreeValue(ctx, exception);
}

static int check_script(JSContext *ctx, const char *path)
{
	char *source;
	size_t len;
	JSValue val;
	JSValue global = JS_UNDEFINED;
	JSValue fn = JS_UNDEFINED;
	int rc = 0;

	source = file_read_all_copy(path, &len);
	if (!source)
		return -ENOENT;
	val = JS_Eval(ctx, source, len, path, JS_EVAL_TYPE_GLOBAL);
	free(source);
	if (JS_IsException(val)) {
		print_exception(ctx, 1);
		return -EINVAL;
	}
	JS_FreeValue(ctx, val);
	global = JS_GetGlobalObject(ctx);
	fn = JS_GetPropertyStr(ctx, global, "run");
	if (!JS_IsFunction(ctx, fn)) {
		print_error_response(1, "script must define global run(args). Example: "
					"async function run(args) { return { ok: true }; }");
		rc = -EINVAL;
	}
	JS_FreeValue(ctx, fn);
	JS_FreeValue(ctx, global);
	return rc;
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

	source = file_read_all_copy(path, &source_len);
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
	fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}\n", id, json_str);
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
	JS_SetRuntimeOpaque(rt, &state);
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
