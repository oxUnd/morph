#ifndef MORPH_RUNTIME_EXTENSIONS_H
#define MORPH_RUNTIME_EXTENSIONS_H

struct runtime_context;

int runtime_extensions_load(struct runtime_context *ctx,
			    const char *front_name);

#endif
