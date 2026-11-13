#include "quickjs.h"
#include "nb/err.h"
#include "nb/cleanup.h"
#include "cJSON.h"
#include "runner_api.h"
#include "js_process.h"
#include "js_fetch.h"
#include "cJSON.h"
#include "nb/file.h"
#include "nb/buf.h"
#include "nb/cleanup.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

size_t runner_capture_limit(JSContext *ctx)
{
	const struct runner_state *state = JS_GetRuntimeOpaque(JS_GetRuntime(ctx));

	return state && state->max_capture_bytes ? state->max_capture_bytes
						: RUNNER_DEFAULT_MAX_CAPTURE;
}

static int runner_capture_configure(struct runner_state *state)
{
	const char *configured = getenv("MORPH_RUNNER_MAX_CAPTURE_BYTES");
	char *end;
	unsigned long long parsed;

	state->max_capture_bytes = RUNNER_DEFAULT_MAX_CAPTURE;
	if (!configured)
		return 0;
	if (!*configured || *configured < '0' || *configured > '9')
		return NB_ERR_INVAL;
	errno = 0;
	parsed = strtoull(configured, &end, 10);
	if (errno || *end || !parsed || parsed > INT_MAX)
		return NB_ERR_INVAL;
	state->max_capture_bytes = (size_t)parsed;
	return 0;
}

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
	nb_buf file_value_1 NB_CLEANUP(nb_buf_destroy) = {0};
	nb_buf file_value_2 NB_CLEANUP(nb_buf_destroy) = {0};

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
	if (nb_path_is_absolute(path)) {
		candidate = strdup(path);
	} else {
		root = getenv(root_env);
		if (!root || !*root) {
			JS_ThrowInternalError(ctx, "dynamic tool %s directory is unavailable",
					      write ? "output" : "work");
			return NULL;
		}
		candidate = (nb_path_join(&file_value_1, root, path) == NB_OK
				     ? strdup(nb_buf_cstr(&file_value_1))
				     : NULL);
	}
	if (!candidate) {
		JS_ThrowOutOfMemory(ctx);
		return NULL;
	}
	resolved =
		(nb_path_expand(&file_value_2, candidate, NULL) == NB_OK &&
				 nb_path_canonical_missing(
					 &file_value_2, nb_buf_cstr(&file_value_2), NULL) == NB_OK
			 ? strdup(nb_buf_cstr(&file_value_2))
			 : NULL);
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
			return NB_ERR_INVAL;
		if (rc == 0)
			break;
	}
	state = JS_PromiseState(ctx, *value);
	if (state == JS_PROMISE_PENDING) {
		JS_ThrowInternalError(ctx, "promise did not settle");
		return NB_ERR_INVAL;
	}
	result = JS_PromiseResult(ctx, *value);
	JS_FreeValue(ctx, *value);
	if (state == JS_PROMISE_REJECTED) {
		*value = JS_UNDEFINED;
		(void)JS_Throw(ctx, result);
		return NB_ERR_INVAL;
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
	nb_buf buf NB_CLEANUP(nb_buf_cleanup) = {0};

	stack = JS_GetPropertyStr(ctx, exception, "stack");
	if (!JS_IsUndefined(stack) && !JS_IsNull(stack))
		stack_str = JS_ToCString(ctx, stack);
	if ((nb_buf_init(&buf), nb_buf_reserve(&buf, 512) ? 0 : NB_ERR_NOMEM) == 0) {
		if ((nb_buf_append_cstr(&buf, msg ? msg : "JavaScript exception")
			     ? 0
			     : NB_ERR_NOMEM) == 0 &&
		    stack_str && *stack_str &&
		    (nb_buf_append_cstr(&buf, "\nStack:\n") ? 0 : NB_ERR_NOMEM) == 0 &&
		    (nb_buf_append_cstr(&buf, stack_str) ? 0 : NB_ERR_NOMEM) == 0)
			print_error_response(id, nb_buf_cstr(&buf));
		else
			print_error_response(id, msg ? msg : "JavaScript exception");
		nb_buf_cleanup(&buf);
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
	nb_buf file_value_3 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_3 NB_CLEANUP(nb_charp_cleanup) = NULL;

	char *source;
	size_t len;
	JSValue val;
	JSValue global = JS_UNDEFINED;
	JSValue fn = JS_UNDEFINED;
	int rc = 0;

	source = (nb_file_read_all(path, &file_value_3, 0, NULL) == NB_OK &&
				  (file_copy_3 = (char *)malloc(nb_buf_len(&file_value_3) + 1)) !=
					  NULL
			  ? (memcpy(file_copy_3, nb_buf_cstr(&file_value_3),
				    nb_buf_len(&file_value_3) + 1),
			     *(&len) = nb_buf_len(&file_value_3), nb_charp_steal(&file_copy_3))
			  : NULL);
	if (!source)
		return NB_ERR_NOT_FOUND;
	val = JS_Eval(ctx, source, len, path, JS_EVAL_TYPE_GLOBAL);
	free(source);
	if (JS_IsException(val)) {
		print_exception(ctx, 1);
		return NB_ERR_INVAL;
	}
	JS_FreeValue(ctx, val);
	global = JS_GetGlobalObject(ctx);
	fn = JS_GetPropertyStr(ctx, global, "run");
	if (!JS_IsFunction(ctx, fn)) {
		print_error_response(1, "script must define global run(args). Example: "
					"async function run(args) { return { ok: true }; }");
		rc = NB_ERR_INVAL;
	}
	JS_FreeValue(ctx, fn);
	JS_FreeValue(ctx, global);
	return rc;
}

struct runner_script_values {
	JSContext *ctx;
	JSValue json;
	JSValue ret;
	JSValue args;
	JSValue fn;
	JSValue global;
	JSValue request;
};

static void runner_script_values_cleanup(struct runner_script_values *values)
{
	JS_FreeValue(values->ctx, values->json);
	JS_FreeValue(values->ctx, values->ret);
	JS_FreeValue(values->ctx, values->args);
	JS_FreeValue(values->ctx, values->fn);
	JS_FreeValue(values->ctx, values->global);
	JS_FreeValue(values->ctx, values->request);
	values->json = JS_UNDEFINED;
	values->ret = JS_UNDEFINED;
	values->args = JS_UNDEFINED;
	values->fn = JS_UNDEFINED;
	values->global = JS_UNDEFINED;
	values->request = JS_UNDEFINED;
}

static int run_script(JSContext *ctx, const char *path)
{
	nb_buf file_value_4 NB_CLEANUP(nb_buf_destroy) = {0};
	char *file_copy_4 NB_CLEANUP(nb_charp_cleanup) = NULL;

	char *source = NULL;
	char *line NB_CLEANUP(nb_charp_cleanup) = NULL;
	size_t source_len;
	size_t line_cap = 0;
	int id = 1;
	int rc = 0;
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;
	JSValue val = JS_UNDEFINED;
	const char *json_str;
	struct runner_script_values values NB_CLEANUP(runner_script_values_cleanup) = {
		.ctx = ctx,
		.json = JS_UNDEFINED,
		.ret = JS_UNDEFINED,
		.args = JS_UNDEFINED,
		.fn = JS_UNDEFINED,
		.global = JS_UNDEFINED,
		.request = JS_UNDEFINED,
	};

	source = (nb_file_read_all(path, &file_value_4, 0, NULL) == NB_OK &&
				  (file_copy_4 = (char *)malloc(nb_buf_len(&file_value_4) + 1)) !=
					  NULL
			  ? (memcpy(file_copy_4, nb_buf_cstr(&file_value_4),
				    nb_buf_len(&file_value_4) + 1),
			     *(&source_len) = nb_buf_len(&file_value_4),
			     nb_charp_steal(&file_copy_4))
			  : NULL);
	if (!source)
		return NB_ERR_NOT_FOUND;
	val = JS_Eval(ctx, source, source_len, path, JS_EVAL_TYPE_GLOBAL);
	free(source);
	source = NULL;
	if (JS_IsException(val)) {
		print_exception(ctx, id);
		return NB_ERR_INVAL;
	}
	JS_FreeValue(ctx, val);
	val = JS_UNDEFINED;

	if (getline(&line, &line_cap, stdin) < 0) {
		print_error_response(id, "missing JSON-RPC request");
		rc = NB_ERR_INVAL;
		return rc;
	}
	root = cJSON_Parse(line);
	if (!root) {
		print_error_response(id, "invalid JSON-RPC request");
		rc = NB_ERR_INVAL;
		return rc;
	}
	{
		cJSON *id_item = cJSON_GetObjectItem(root, "id");
		if (cJSON_IsNumber(id_item))
			id = id_item->valueint;
	}
	/* Preserve JSON strings verbatim, including embedded NUL characters. */
	values.request = JS_ParseJSON(ctx, line, strlen(line), "<request>");
	if (JS_IsException(values.request)) {
		print_exception(ctx, id);
		return NB_ERR_INVAL;
	}
	values.args = JS_GetPropertyStr(ctx, values.request, "params");
	if (JS_IsUndefined(values.args))
		values.args = JS_NewObject(ctx);
	if (JS_IsException(values.args)) {
		print_exception(ctx, id);
		rc = NB_ERR_INVAL;
		return rc;
	}

	values.global = JS_GetGlobalObject(ctx);
	values.fn = JS_GetPropertyStr(ctx, values.global, "run");
	if (!JS_IsFunction(ctx, values.fn)) {
		print_error_response(id, "script must define global run(args)");
		rc = NB_ERR_INVAL;
		return rc;
	}
	values.ret = JS_Call(ctx, values.fn, JS_UNDEFINED, 1, &values.args);
	if (JS_IsException(values.ret)) {
		print_exception(ctx, id);
		rc = NB_ERR_INVAL;
		return rc;
	}
	if (resolve_promise(ctx, &values.ret) < 0) {
		print_exception(ctx, id);
		rc = NB_ERR_INVAL;
		return rc;
	}
	values.json = JS_JSONStringify(ctx, values.ret, JS_UNDEFINED, JS_UNDEFINED);
	if (JS_IsException(values.json)) {
		print_exception(ctx, id);
		rc = NB_ERR_INVAL;
		return rc;
	}
	if (JS_IsUndefined(values.json))
		json_str = "null";
	else
		json_str = JS_ToCString(ctx, values.json);
	if (!json_str) {
		print_exception(ctx, id);
		rc = NB_ERR_INVAL;
		return rc;
	}
	fprintf(stdout, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}\n", id, json_str);
	if (!JS_IsUndefined(values.json))
		JS_FreeCString(ctx, json_str);

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
	memset(&state, 0, sizeof(state));
	if (runner_capture_configure(&state) != 0) {
		print_error_response(1, "MORPH_RUNNER_MAX_CAPTURE_BYTES must be an integer "
					"between 1 and INT_MAX.");
		return 1;
	}
	if (js_media_init() < 0) {
		fprintf(stderr, "failed to initialize media bindings\n");
		return 1;
	}
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
