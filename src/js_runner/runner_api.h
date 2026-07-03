#ifndef RUNNER_API_H
#define RUNNER_API_H

#include "quickjs.h"

int runner_has_cap(const char *cap);
int runner_list_allows(const char *env_name, const char *value);
JSValue runner_throw_cap(JSContext *ctx, const char *cap);

int js_media_init(void);
void js_media_shutdown(void);
void install_media_api(JSContext *ctx);

#endif
