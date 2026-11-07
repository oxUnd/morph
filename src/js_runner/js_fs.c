#include "js_fs.h"
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

static JSValue js_fs_read_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	const char *path;
	char *resolved;
	char *data;
	size_t len;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "readText(path) requires path");
	path = JS_ToCString(ctx, argv[0]);
	if (!path)
		return JS_EXCEPTION;
	resolved = runner_authorize_path(ctx, path, 0);
	JS_FreeCString(ctx, path);
	if (!resolved)
		return JS_EXCEPTION;
	data = file_read_all(resolved, &len);
	free(resolved);
	if (!data)
		return JS_ThrowInternalError(ctx, "failed to read file");
	JSValue out = JS_NewStringLen(ctx, data, len);
	free(data);
	return out;
}

static JSValue js_fs_write_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	const char *path;
	char *resolved;
	const char *text;
	size_t len;
	int rc;

	(void)this_val;
	if (argc < 2)
		return JS_ThrowTypeError(ctx, "writeText(path, text) requires two args");
	path = JS_ToCString(ctx, argv[0]);
	if (!path)
		return JS_EXCEPTION;
	resolved = runner_authorize_path(ctx, path, 1);
	JS_FreeCString(ctx, path);
	if (!resolved)
		return JS_EXCEPTION;
	text = JS_ToCStringLen(ctx, &len, argv[1]);
	if (!text) {
		free(resolved);
		return JS_EXCEPTION;
	}
	rc = file_write_all(resolved, text, len);
	free(resolved);
	JS_FreeCString(ctx, text);
	if (rc < 0)
		return JS_ThrowInternalError(ctx, "failed to write file");
	return JS_NewBool(ctx, 1);
}

static JSValue js_fs_read_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
static JSValue js_fs_write_file_sync(JSContext *ctx, JSValueConst this_val, int argc,
				     JSValueConst *argv);

static JSValue js_fs_read_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
	const char *path;
	char *resolved;
	char *data;
	size_t len;
	JSValue out;

	(void)this_val;
	if (argc < 1)
		return JS_ThrowTypeError(ctx, "readFile(path)");
	path = JS_ToCString(ctx, argv[0]);
	if (!path)
		return JS_EXCEPTION;
	resolved = runner_authorize_path(ctx, path, 0);
	JS_FreeCString(ctx, path);
	if (!resolved)
		return JS_EXCEPTION;
	data = file_read_all(resolved, &len);
	free(resolved);
	if (!data)
		return JS_ThrowInternalError(ctx, "failed to read file");
	out = JS_NewArrayBufferCopy(ctx, (const uint8_t *)data, len);
	free(data);
	return out;
}

static JSValue js_fs_write_file_sync(JSContext *ctx, JSValueConst this_val, int argc,
				     JSValueConst *argv)
{
	const char *path;
	char *resolved;
	const char *text;
	uint8_t *bytes;
	size_t len = 0;
	int rc;

	(void)this_val;
	if (argc < 2)
		return JS_ThrowTypeError(ctx, "writeFileSync(path, data)");
	path = JS_ToCString(ctx, argv[0]);
	if (!path)
		return JS_EXCEPTION;
	resolved = runner_authorize_path(ctx, path, 1);
	JS_FreeCString(ctx, path);
	if (!resolved)
		return JS_EXCEPTION;
	bytes = js_values_buffer_bytes(ctx, argv[1], &len);
	if (bytes) {
		rc = file_write_all(resolved, (const char *)bytes, len);
		free(resolved);
		if (rc < 0)
			return JS_ThrowInternalError(ctx, "failed to write file: %s",
						     strerror(-rc));
		return JS_UNDEFINED;
	}
	text = JS_ToCStringLen(ctx, &len, argv[1]);
	if (!text) {
		free(resolved);
		return JS_EXCEPTION;
	}
	rc = file_write_all(resolved, text, len);
	free(resolved);
	JS_FreeCString(ctx, text);
	if (rc < 0)
		return JS_ThrowInternalError(ctx, "failed to write file: %s", strerror(-rc));
	return JS_UNDEFINED;
}

void js_fs_install(JSContext *ctx, JSValueConst morph)
{
	JSValue fs = JS_GetPropertyStr(ctx, morph, "fs");

	if (JS_IsUndefined(fs) || JS_IsNull(fs)) {
		JS_FreeValue(ctx, fs);
		fs = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, morph, "fs", JS_DupValue(ctx, fs));
	}
	JS_SetPropertyStr(ctx, fs, "readText",
			  JS_NewCFunction(ctx, js_fs_read_text, "readText", 1));
	JS_SetPropertyStr(ctx, fs, "writeText",
			  JS_NewCFunction(ctx, js_fs_write_text, "writeText", 2));
	JS_SetPropertyStr(ctx, fs, "readFile",
			  JS_NewCFunction(ctx, js_fs_read_file, "readFile", 1));
	JS_SetPropertyStr(ctx, fs, "writeFile",
			  JS_NewCFunction(ctx, js_fs_write_file_sync, "writeFile", 2));
	JS_FreeValue(ctx, fs);
}
