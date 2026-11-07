#include "runner_api.h"
#include "js_image.h"
#include "js_canvas.h"
#include "js_fs.h"
#include "js_wasm.h"
#include <errno.h>
#include <vips/vips.h>

static JSValue js_require_disabled(JSContext *ctx, JSValueConst this_val, int argc,
				   JSValueConst *argv)
{
	(void)this_val;
	(void)argc;
	(void)argv;
	return JS_ThrowTypeError(ctx, "require() is disabled in dynamic tools; use morph.image, "
				      "morph.canvas, and morph.fs");
}

int js_media_init(void)
{
	if (VIPS_INIT("morph-js-runner"))
		return -EINVAL;
	return 0;
}

void js_media_shutdown(void)
{
	vips_shutdown();
}

void install_media_api(JSContext *ctx)
{
	JSValue global = JS_GetGlobalObject(ctx);
	JSValue morph = JS_GetPropertyStr(ctx, global, "morph");

	JS_SetPropertyStr(ctx, global, "require",
			  JS_NewCFunction(ctx, js_require_disabled, "require", 1));
	if (JS_IsUndefined(morph) || JS_IsNull(morph)) {
		JS_FreeValue(ctx, morph);
		morph = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, global, "morph", JS_DupValue(ctx, morph));
	}
	js_image_install(ctx, morph);
	js_canvas_install(ctx, morph);
	js_fs_install(ctx, morph);
	js_wasm_install(ctx, global);
	JS_FreeValue(ctx, morph);
	JS_FreeValue(ctx, global);
}
