#include "sapi/cli/adapters/input/callbacks.h"
#include "sapi/cli/adapters/input/state.h"
#include "sapi/cli/application_internal.h"
#include "util/error.h"
#include <ffi.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

enum cli_callbacks_kind {
	CLI_CALLBACKS_CALLBACK_KEY,
	CLI_CALLBACKS_CALLBACK_DISPLAY,
	CLI_CALLBACKS_CALLBACK_LINE,
	CLI_CALLBACKS_CALLBACK_GETC,
	CLI_CALLBACKS_CALLBACK_COMPLETE,
	CLI_CALLBACKS_CALLBACK_GENERATE
};

union cli_callbacks_target {
	int (*key)(struct cli_application *, int, int);
	void (*display)(struct cli_application *);
	void (*line)(struct cli_application *, char *);
	int (*getc)(struct cli_application *, FILE *);
	char **(*complete)(struct cli_application *, const char *, int, int);
	char *(*generate)(struct cli_application *, const char *, int);
};

struct cli_callback {
	struct cli_callback *next;
	struct cli_application *ctx;
	enum cli_callbacks_kind kind;
	union cli_callbacks_target target;
	ffi_closure *closure;
	ffi_cif cif;
	ffi_type *arguments[3];
	void *code;
};

static void cli_callbacks_dispatch(ffi_cif *cif, void *result, void **args, void *opaque)
{
	struct cli_callback *binding = opaque;
	struct cli_application *ctx = binding->ctx;

	(void)cif;
	switch (binding->kind) {
	case CLI_CALLBACKS_CALLBACK_KEY:
		*(ffi_arg *)result =
			(ffi_arg)binding->target.key(ctx, *(int *)args[0], *(int *)args[1]);
		break;
	case CLI_CALLBACKS_CALLBACK_DISPLAY:
		binding->target.display(ctx);
		break;
	case CLI_CALLBACKS_CALLBACK_LINE:
		binding->target.line(ctx, *(char **)args[0]);
		break;
	case CLI_CALLBACKS_CALLBACK_GETC:
		*(ffi_arg *)result = (ffi_arg)binding->target.getc(ctx, *(FILE **)args[0]);
		break;
	case CLI_CALLBACKS_CALLBACK_COMPLETE:
		*(char ***)result = binding->target.complete(ctx, *(const char **)args[0],
							     *(int *)args[1], *(int *)args[2]);
		break;
	case CLI_CALLBACKS_CALLBACK_GENERATE:
		*(char **)result =
			binding->target.generate(ctx, *(const char **)args[0], *(int *)args[1]);
		break;
	}
}

static void *cli_callbacks_bind_callback(struct cli_application *ctx, enum cli_callbacks_kind kind,
					 union cli_callbacks_target target)
{
	struct cli_callback *binding;
	ffi_type *result = &ffi_type_void;
	unsigned int count = 0;

	if (!ctx || cli_input_init(ctx))
		return NULL;
	for (binding = ctx->input->callbacks; binding; binding = binding->next) {
		if (binding->kind == kind && !memcmp(&binding->target, &target, sizeof(target)))
			return binding->code;
	}
	binding = calloc(1, sizeof(*binding));
	if (!binding) {
		ctx->input->callback_error = -ENOMEM;
		return NULL;
	}
	binding->ctx = ctx;
	binding->kind = kind;
	binding->target = target;
	if (kind == CLI_CALLBACKS_CALLBACK_KEY) {
		count = 2;
		binding->arguments[0] = binding->arguments[1] = &ffi_type_sint;
		result = &ffi_type_sint;
	} else if (kind != CLI_CALLBACKS_CALLBACK_DISPLAY) {
		count = 1;
		binding->arguments[0] = &ffi_type_pointer;
		if (kind == CLI_CALLBACKS_CALLBACK_GETC)
			result = &ffi_type_sint;
		if (kind == CLI_CALLBACKS_CALLBACK_COMPLETE ||
		    kind == CLI_CALLBACKS_CALLBACK_GENERATE) {
			count = kind == CLI_CALLBACKS_CALLBACK_COMPLETE ? 3u : 2u;
			binding->arguments[1] = binding->arguments[2] = &ffi_type_sint;
			result = &ffi_type_pointer;
		}
	}
	binding->closure = ffi_closure_alloc(sizeof(*binding->closure), &binding->code);
	if (!binding->closure ||
	    ffi_prep_cif(&binding->cif, FFI_DEFAULT_ABI, count, result, binding->arguments) !=
		    FFI_OK ||
	    ffi_prep_closure_loc(binding->closure, &binding->cif, cli_callbacks_dispatch, binding,
				 binding->code) != FFI_OK) {
		if (binding->closure)
			ffi_closure_free(binding->closure);
		free(binding);
		ctx->input->callback_error = -ENOMEM;
		return NULL;
	}
	binding->next = ctx->input->callbacks;
	ctx->input->callbacks = binding;
	return binding->code;
}

#define CLI_CALLBACKS_DEFINE_BINDING(name, member, kind, return_type, original_args, bound_args)   \
	return_type(*cli_callback_##name(struct cli_application *ctx,                              \
					 return_type(*callback) bound_args)) original_args         \
	{                                                                                          \
		union cli_callbacks_target target = {0};                                           \
		return_type(*function) original_args;                                              \
		target.member = callback;                                                          \
		void *code = cli_callbacks_bind_callback(ctx, kind, target);                       \
		_Static_assert(sizeof(function) == sizeof(code), "incompatible callback ABI");     \
		memcpy(&function, &code, sizeof(function));                                        \
		return function;                                                                   \
	}

CLI_CALLBACKS_DEFINE_BINDING(key, key, CLI_CALLBACKS_CALLBACK_KEY, int, (int, int),
			     (struct cli_application *, int, int))
CLI_CALLBACKS_DEFINE_BINDING(display, display, CLI_CALLBACKS_CALLBACK_DISPLAY, void, (void),
			     (struct cli_application *))
CLI_CALLBACKS_DEFINE_BINDING(line, line, CLI_CALLBACKS_CALLBACK_LINE, void, (char *),
			     (struct cli_application *, char *))
CLI_CALLBACKS_DEFINE_BINDING(getc, getc, CLI_CALLBACKS_CALLBACK_GETC, int, (FILE *),
			     (struct cli_application *, FILE *))
CLI_CALLBACKS_DEFINE_BINDING(complete, complete, CLI_CALLBACKS_CALLBACK_COMPLETE, char **,
			     (const char *, int, int),
			     (struct cli_application *, const char *, int, int))
CLI_CALLBACKS_DEFINE_BINDING(generate, generate, CLI_CALLBACKS_CALLBACK_GENERATE, char *,
			     (const char *, int), (struct cli_application *, const char *, int))

void cli_callbacks_cleanup(struct cli_application *ctx)
{
	if (!ctx || !ctx->input)
		return;
	while (ctx->input->callbacks) {
		struct cli_callback *binding = ctx->input->callbacks;

		ctx->input->callbacks = binding->next;
		ffi_closure_free(binding->closure);
		free(binding);
	}
	ctx->input->callback_error = 0;
}
