#include "js_wasm.h"
#include "util/cleanup.h"
#include "js_values.h"
#include "util/buf.h"
#include "util/file.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm3.h"
#define WASM_STACK_SIZE (64 * 1024)
#define WASM_MAX_EXPORTS 128
struct wasm_module {
	uint8_t *bytes;
	size_t len;
	char exports[WASM_MAX_EXPORTS][64];
	int export_count;
	int has_memory;
};

struct wasm_instance {
	IM3Environment env;
	IM3Runtime runtime;
	uint8_t *bytes;
	size_t len;
	char exports[WASM_MAX_EXPORTS][64];
	int export_count;
};

static int require_wasm(JSContext *ctx);
static void wasm_instance_owner_cleanup(struct wasm_instance **owner);
static void wasm_module_finalizer(JSRuntime *rt, JSValue val);
static void wasm_instance_finalizer(JSRuntime *rt, JSValue val);
static int read_u32_leb(const uint8_t *buf, size_t len, size_t *pos, uint32_t *out);
static int parse_wasm_exports(struct wasm_module *module);
static JSValue new_wasm_module(JSContext *ctx, const uint8_t *bytes, size_t len);
static JSValue js_wasm_module_ctor(JSContext *ctx, JSValueConst new_target, int argc,
				   JSValueConst *argv);
static void array_buffer_free(JSRuntime *rt, void *opaque, void *ptr);
static JSValue wasm_export_call(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
				int magic, JSValue *func_data);
static JSValue build_wasm_instance(JSContext *ctx, struct wasm_module *module);
static JSValue js_wasm_instance_ctor(JSContext *ctx, JSValueConst new_target, int argc,
				     JSValueConst *argv);
static JSValue js_wasm_compile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_wasm_instantiate(JSContext *ctx, JSValueConst this_val, int argc,
				   JSValueConst *argv);
static JSValue js_wasm_memory_ctor(JSContext *ctx, JSValueConst new_target, int argc,
				   JSValueConst *argv);

static int require_wasm(JSContext *ctx)
{
	if (!runner_has_cap("wasm")) {
		runner_throw_cap(ctx, "wasm");
		return 0;
	}
	return 1;
}

static void wasm_module_finalizer(JSRuntime *rt, JSValue val)
{
	struct wasm_module *module = JS_GetOpaque(val, JS_GetClassID(val));

	(void)rt;
	if (!module)
		return;
	free(module->bytes);
	free(module);
}

static void wasm_instance_finalizer(JSRuntime *rt, JSValue val)
{
	struct wasm_instance *inst = JS_GetOpaque(val, JS_GetClassID(val));

	(void)rt;
	wasm_instance_owner_cleanup(&inst);
}

static int read_u32_leb(const uint8_t *buf, size_t len, size_t *pos, uint32_t *out)
{
	uint32_t value = 0;
	int shift = 0;

	while (*pos < len && shift < 35) {
		uint8_t byte = buf[(*pos)++];
		value |= (uint32_t)(byte & 0x7f) << shift;
		if ((byte & 0x80) == 0) {
			*out = value;
			return 0;
		}
		shift += 7;
	}
	return -EINVAL;
}

static int parse_wasm_exports(struct wasm_module *module)
{
	const uint8_t *buf = module->bytes;
	size_t pos = 8;
	uint32_t count;

	if (module->len < 8 || memcmp(buf, "\0asm", 4) != 0)
		return -EINVAL;
	while (pos < module->len) {
		uint8_t id;
		uint32_t size;
		size_t end;

		id = buf[pos++];
		if (read_u32_leb(buf, module->len, &pos, &size) < 0)
			return -EINVAL;
		end = pos + size;
		if (end > module->len)
			return -EINVAL;
		if (id != 7) {
			pos = end;
			continue;
		}
		if (read_u32_leb(buf, end, &pos, &count) < 0)
			return -EINVAL;
		for (uint32_t i = 0; i < count && pos < end; i++) {
			uint32_t name_len;
			uint8_t kind;
			uint32_t index;

			if (read_u32_leb(buf, end, &pos, &name_len) < 0 || pos + name_len > end)
				return -EINVAL;
			if (name_len > 0 && module->export_count < WASM_MAX_EXPORTS) {
				size_t keep = name_len;
				if (keep >= sizeof(module->exports[0]))
					keep = sizeof(module->exports[0]) - 1;
				memcpy(module->exports[module->export_count], buf + pos, keep);
				module->exports[module->export_count][keep] = '\0';
			}
			pos += name_len;
			if (pos >= end)
				return -EINVAL;
			kind = buf[pos++];
			if (read_u32_leb(buf, end, &pos, &index) < 0)
				return -EINVAL;
			(void)index;
			if (kind == 0 && module->export_count < WASM_MAX_EXPORTS)
				module->export_count++;
			else if (kind == 2)
				module->has_memory = 1;
		}
		return 0;
	}
	return 0;
}

static JSValue new_wasm_module(JSContext *ctx, const uint8_t *bytes, size_t len)
{
	struct wasm_module *module;
	JSValue obj;

	module = calloc(1, sizeof(*module));
	if (!module)
		return JS_ThrowOutOfMemory(ctx);
	module->bytes = malloc(len);
	if (!module->bytes) {
		free(module);
		return JS_ThrowOutOfMemory(ctx);
	}
	memcpy(module->bytes, bytes, len);
	module->len = len;
	if (parse_wasm_exports(module) < 0) {
		free(module->bytes);
		free(module);
		return JS_ThrowTypeError(ctx, "invalid wasm module");
	}
	obj = JS_NewObjectClass(ctx, js_values_state_borrow(ctx)->wasm_module_class_id);
	if (JS_IsException(obj)) {
		free(module->bytes);
		free(module);
		return obj;
	}
	JS_SetOpaque(obj, module);
	return obj;
}

static JSValue js_wasm_module_ctor(JSContext *ctx, JSValueConst new_target, int argc,
				   JSValueConst *argv)
{
	uint8_t *bytes;
	size_t len;

	(void)new_target;
	if (!require_wasm(ctx))
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "WebAssembly.Module(bytes)");
	bytes = JS_GetArrayBuffer(ctx, &len, argv[0]);
	if (!bytes)
		return JS_ThrowTypeError(ctx, "wasm bytes must be ArrayBuffer");
	return new_wasm_module(ctx, bytes, len);
}

static void array_buffer_free(JSRuntime *rt, void *opaque, void *ptr)
{
	(void)rt;
	(void)opaque;
	free(ptr);
}

static JSValue wasm_export_call(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
				int magic, JSValue *func_data)
{
	struct wasm_instance *inst;
	IM3Function fn;
	const char *name;
	const char *args[16];
	char arg_storage[16][64];
	uint32_t retc;
	uint64_t ret64 = 0;
	const void *rets[1];
	M3Result res;

	(void)this_val;
	inst = JS_GetOpaque2(ctx, func_data[0],
			     js_values_state_borrow(ctx)->wasm_instance_class_id);
	name = JS_ToCString(ctx, func_data[1]);
	if (!inst || !name)
		return JS_EXCEPTION;
	if (argc > 16) {
		JS_FreeCString(ctx, name);
		return JS_ThrowRangeError(ctx, "too many wasm arguments");
	}
	for (int i = 0; i < argc; i++) {
		double value = 0.0;
		(void)JS_ToFloat64(ctx, &value, argv[i]);
		snprintf(arg_storage[i], sizeof(arg_storage[i]), "%.17g", value);
		args[i] = arg_storage[i];
	}
	res = m3_FindFunction(&fn, inst->runtime, name);
	if (res) {
		JS_FreeCString(ctx, name);
		return JS_ThrowInternalError(ctx, "wasm export not found");
	}
	res = m3_CallArgv(fn, (uint32_t)argc, args);
	if (res) {
		JS_FreeCString(ctx, name);
		return JS_ThrowInternalError(ctx, "wasm call failed: %s", res);
	}
	retc = m3_GetRetCount(fn);
	if (retc == 0) {
		JS_FreeCString(ctx, name);
		return JS_UNDEFINED;
	}
	rets[0] = &ret64;
	res = m3_GetResults(fn, 1, rets);
	JS_FreeCString(ctx, name);
	if (res)
		return JS_ThrowInternalError(ctx, "wasm result failed");
	(void)magic;
	return JS_NewInt64(ctx, (int64_t)ret64);
}

static void wasm_instance_owner_cleanup(struct wasm_instance **owner)
{
	struct wasm_instance *inst = *owner;

	if (!inst)
		return;
	if (inst->runtime)
		m3_FreeRuntime(inst->runtime);
	if (inst->env)
		m3_FreeEnvironment(inst->env);
	free(inst->bytes);
	free(inst);
	*owner = NULL;
}

static void wasm_module_owner_cleanup(IM3Module *owner)
{
	if (*owner)
		m3_FreeModule(*owner);
	*owner = NULL;
}

static JSValue build_wasm_instance(JSContext *ctx, struct wasm_module *module)
{
	struct wasm_instance *inst MORPH_CLEANUP(wasm_instance_owner_cleanup) = NULL;
	IM3Module m3_module MORPH_CLEANUP(wasm_module_owner_cleanup) = NULL;
	M3Result res;
	JSValue obj;
	JSValue exports;

	inst = calloc(1, sizeof(*inst));
	if (!inst)
		return JS_ThrowOutOfMemory(ctx);
	inst->bytes = malloc(module->len);
	if (!inst->bytes)
		return JS_ThrowOutOfMemory(ctx);
	memcpy(inst->bytes, module->bytes, module->len);
	inst->len = module->len;
	memcpy(inst->exports, module->exports, sizeof(inst->exports));
	inst->export_count = module->export_count;
	inst->env = m3_NewEnvironment();
	if (!inst->env)
		return JS_ThrowOutOfMemory(ctx);
	inst->runtime = m3_NewRuntime(inst->env, WASM_STACK_SIZE, NULL);
	if (!inst->runtime)
		return JS_ThrowOutOfMemory(ctx);
	res = m3_ParseModule(inst->env, &m3_module, inst->bytes, (uint32_t)inst->len);
	if (res)
		return JS_ThrowTypeError(ctx, "failed to instantiate wasm: %s", res);
	res = m3_LoadModule(inst->runtime, m3_module);
	m3_module = NULL;
	if (res)
		return JS_ThrowTypeError(ctx, "failed to instantiate wasm: %s", res);
	obj = JS_NewObjectClass(ctx, js_values_state_borrow(ctx)->wasm_instance_class_id);
	if (JS_IsException(obj))
		return JS_ThrowOutOfMemory(ctx);
	JS_SetOpaque(obj, inst);
	exports = JS_NewObject(ctx);
	for (int i = 0; i < inst->export_count; i++) {
		JSValue data[2];
		JSValue fn;

		data[0] = JS_DupValue(ctx, obj);
		data[1] = JS_NewString(ctx, inst->exports[i]);
		fn = JS_NewCFunctionData(ctx, wasm_export_call, 0, 0, 2, data);
		JS_FreeValue(ctx, data[0]);
		JS_FreeValue(ctx, data[1]);
		JS_SetPropertyStr(ctx, exports, inst->exports[i], fn);
	}
	JS_SetPropertyStr(ctx, obj, "exports", exports);
	inst = NULL;
	return obj;
}

static JSValue js_wasm_instance_ctor(JSContext *ctx, JSValueConst new_target, int argc,
				     JSValueConst *argv)
{
	struct wasm_module *module;

	(void)new_target;
	if (!require_wasm(ctx))
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "WebAssembly.Instance(module)");
	module = JS_GetOpaque2(ctx, argv[0], js_values_state_borrow(ctx)->wasm_module_class_id);
	if (!module)
		return JS_EXCEPTION;
	return build_wasm_instance(ctx, module);
}

static JSValue js_wasm_compile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	(void)this_val;
	return js_wasm_module_ctor(ctx, JS_UNDEFINED, argc, argv);
}

static JSValue js_wasm_instantiate(JSContext *ctx, JSValueConst this_val, int argc,
				   JSValueConst *argv)
{
	struct wasm_module *module;
	JSValue module_obj = JS_UNDEFINED;
	JSValue instance;
	JSValue out;

	(void)this_val;
	if (!require_wasm(ctx))
		return JS_EXCEPTION;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "WebAssembly.instantiate(bytes)");
	module = JS_GetOpaque(argv[0], js_values_state_borrow(ctx)->wasm_module_class_id);
	if (module) {
		instance = build_wasm_instance(ctx, module);
		return instance;
	}
	module_obj = js_wasm_module_ctor(ctx, JS_UNDEFINED, argc, argv);
	if (JS_IsException(module_obj))
		return module_obj;
	module = JS_GetOpaque2(ctx, module_obj, js_values_state_borrow(ctx)->wasm_module_class_id);
	if (!module) {
		JS_FreeValue(ctx, module_obj);
		return JS_EXCEPTION;
	}
	instance = build_wasm_instance(ctx, module);
	if (JS_IsException(instance)) {
		JS_FreeValue(ctx, module_obj);
		return instance;
	}
	out = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, out, "module", module_obj);
	JS_SetPropertyStr(ctx, out, "instance", instance);
	return out;
}

static JSValue js_wasm_memory_ctor(JSContext *ctx, JSValueConst new_target, int argc,
				   JSValueConst *argv)
{
	JSValue obj;
	JSValue desc;
	JSValue initial_val;
	int32_t initial = 1;
	size_t len;
	uint8_t *zero;

	(void)new_target;
	if (!require_wasm(ctx))
		return JS_EXCEPTION;
	if (argc > 0 && JS_IsObject(argv[0])) {
		desc = argv[0];
		initial_val = JS_GetPropertyStr(ctx, desc, "initial");
		(void)JS_ToInt32(ctx, &initial, initial_val);
		JS_FreeValue(ctx, initial_val);
	}
	if (initial <= 0 || initial > 256)
		return JS_ThrowRangeError(ctx, "invalid memory size");
	len = (size_t)initial * 65536;
	zero = calloc(1, len);
	if (!zero)
		return JS_ThrowOutOfMemory(ctx);
	obj = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, obj, "buffer",
			  JS_NewArrayBuffer(ctx, zero, len, array_buffer_free, NULL, 0));
	return obj;
}

void js_wasm_install(JSContext *ctx, JSValueConst global)
{
	JSRuntime *rt = JS_GetRuntime(ctx);
	struct runner_state *state = js_values_state_borrow(ctx);
	JSClassDef module_def = {"WasmModule", .finalizer = wasm_module_finalizer};
	JSClassDef instance_def = {"WasmInstance", .finalizer = wasm_instance_finalizer};
	JSValue wasm = JS_NewObject(ctx);

	if (!state->wasm_module_class_id)
		JS_NewClassID(&state->wasm_module_class_id);
	if (!state->wasm_instance_class_id)
		JS_NewClassID(&state->wasm_instance_class_id);
	JS_NewClass(rt, state->wasm_module_class_id, &module_def);
	JS_NewClass(rt, state->wasm_instance_class_id, &instance_def);
	JS_SetPropertyStr(
		ctx, wasm, "Module",
		JS_NewCFunction2(ctx, js_wasm_module_ctor, "Module", 1, JS_CFUNC_constructor, 0));
	JS_SetPropertyStr(ctx, wasm, "Instance",
			  JS_NewCFunction2(ctx, js_wasm_instance_ctor, "Instance", 2,
					   JS_CFUNC_constructor, 0));
	JS_SetPropertyStr(
		ctx, wasm, "Memory",
		JS_NewCFunction2(ctx, js_wasm_memory_ctor, "Memory", 1, JS_CFUNC_constructor, 0));
	JS_SetPropertyStr(ctx, wasm, "compile",
			  JS_NewCFunction(ctx, js_wasm_compile, "compile", 1));
	JS_SetPropertyStr(ctx, wasm, "instantiate",
			  JS_NewCFunction(ctx, js_wasm_instantiate, "instantiate", 2));
	JS_SetPropertyStr(ctx, global, "WebAssembly", wasm);
}
