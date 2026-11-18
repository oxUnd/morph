#include "sapi/cli/adapters/input/callbacks.h"
#include "sapi/cli/adapters/input/state.h"
#include "sapi/cli/application.h"
#include "errors.h"
#include <ffi.h>
#include <stdlib.h>
#include <string.h>

struct cli_callback {
	struct cli_callback *next;
	struct cli_application *ctx;
	int (*reader)(struct cli_application *, FILE *);
	ffi_closure *closure;
	ffi_cif cif;
	ffi_type *argument;
	void *code;
};

static void cli_callbacks_read(ffi_cif *cif, void *result, void **args, void *data)
{
	struct cli_callback *binding = data;
	(void)cif;
	*(ffi_arg *)result = (ffi_arg)binding->reader(binding->ctx, *(FILE **)args[0]);
}

int (*cli_callback_getc(struct cli_application *ctx,
			int (*reader)(struct cli_application *, FILE *)))(FILE *)
{
	if (!ctx || !reader || cli_input_init(ctx))
		return NULL;
	struct cli_callback *binding = ctx->input->callbacks;
	while (binding && binding->reader != reader)
		binding = binding->next;
	if (!binding) {
		binding = calloc(1, sizeof(*binding));
		if (!binding) {
			ctx->input->callback_error = NB_ERR_NOMEM;
			return NULL;
		}
		binding->ctx = ctx;
		binding->reader = reader;
		binding->argument = &ffi_type_pointer;
		binding->closure = ffi_closure_alloc(sizeof(*binding->closure), &binding->code);
		if (!binding->closure ||
		    ffi_prep_cif(&binding->cif, FFI_DEFAULT_ABI, 1, &ffi_type_sint,
				 &binding->argument) != FFI_OK ||
		    ffi_prep_closure_loc(binding->closure, &binding->cif, cli_callbacks_read,
					 binding, binding->code) != FFI_OK) {
			if (binding->closure)
				ffi_closure_free(binding->closure);
			free(binding);
			ctx->input->callback_error = NB_ERR_NOMEM;
			return NULL;
		}
		binding->next = ctx->input->callbacks;
		ctx->input->callbacks = binding;
	}
	int (*callback)(FILE *);
	_Static_assert(sizeof(callback) == sizeof(binding->code), "incompatible callback ABI");
	memcpy(&callback, &binding->code, sizeof(callback));
	return callback;
}

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
