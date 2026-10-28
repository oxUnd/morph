#ifndef RUNNER_API_H
#define RUNNER_API_H

#include "quickjs.h"
#include <time.h>

struct runner_state {
	time_t deadline;
	JSClassID sharp_class_id;
	JSClassID canvas_class_id;
	JSClassID image_data_class_id;
	JSClassID wasm_module_class_id;
	JSClassID wasm_instance_class_id;
};

int runner_has_cap(const char *cap);
int runner_list_allows(const char *env_name, const char *value);
JSValue runner_throw_cap(JSContext *ctx, const char *cap);
char *runner_authorize_path(JSContext *ctx, const char *path, int write);

int js_media_init(void);
void js_media_shutdown(void);
void install_media_api(JSContext *ctx);

#endif
