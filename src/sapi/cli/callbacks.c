#include "callbacks.h"
#include "internal.h"
#include "util/error.h"
#include "util/child_io.h"
#include "util/cancel.h"
#include <ffi.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#ifdef HAVE_READLINE
#include <readline/readline.h>
#endif

enum callback_kind {
	CALLBACK_KEY,
	CALLBACK_DISPLAY,
	CALLBACK_LINE,
	CALLBACK_GETC,
	CALLBACK_COMPLETE,
	CALLBACK_GENERATE
};

union callback_target {
	int (*key)(struct cli_context *, int, int);
	void (*display)(struct cli_context *);
	void (*line)(struct cli_context *, char *);
	int (*getc)(struct cli_context *, FILE *);
	char **(*complete)(struct cli_context *, const char *, int, int);
	char *(*generate)(struct cli_context *, const char *, int);
};

struct cli_callback {
	struct cli_callback *next;
	struct cli_context *ctx;
	enum callback_kind kind;
	union callback_target target;
	ffi_closure *closure;
	ffi_cif cif;
	ffi_type *arguments[3];
	void *code;
};

static void dispatch(ffi_cif *cif, void *result, void **args, void *opaque)
{
	struct cli_callback *binding = opaque;
	struct cli_context *ctx = binding->ctx;

	(void)cif;
	switch (binding->kind) {
	case CALLBACK_KEY:
		*(ffi_arg *)result =
			(ffi_arg)binding->target.key(ctx, *(int *)args[0], *(int *)args[1]);
		break;
	case CALLBACK_DISPLAY:
		binding->target.display(ctx);
		break;
	case CALLBACK_LINE:
		binding->target.line(ctx, *(char **)args[0]);
		break;
	case CALLBACK_GETC:
		*(ffi_arg *)result = (ffi_arg)binding->target.getc(ctx, *(FILE **)args[0]);
		break;
	case CALLBACK_COMPLETE:
		*(char ***)result = binding->target.complete(ctx, *(const char **)args[0],
							     *(int *)args[1], *(int *)args[2]);
		break;
	case CALLBACK_GENERATE:
		*(char **)result =
			binding->target.generate(ctx, *(const char **)args[0], *(int *)args[1]);
		break;
	}
}

static void *bind_callback(struct cli_context *ctx, enum callback_kind kind,
			   union callback_target target)
{
	struct cli_callback *binding;
	ffi_type *result = &ffi_type_void;
	unsigned int count = 0;

	if (!ctx)
		return NULL;
	for (binding = ctx->callbacks; binding; binding = binding->next) {
		if (binding->kind == kind && !memcmp(&binding->target, &target, sizeof(target)))
			return binding->code;
	}
	binding = calloc(1, sizeof(*binding));
	if (!binding) {
		ctx->callback_error = -ENOMEM;
		return NULL;
	}
	binding->ctx = ctx;
	binding->kind = kind;
	binding->target = target;
	if (kind == CALLBACK_KEY) {
		count = 2;
		binding->arguments[0] = binding->arguments[1] = &ffi_type_sint;
		result = &ffi_type_sint;
	} else if (kind != CALLBACK_DISPLAY) {
		count = 1;
		binding->arguments[0] = &ffi_type_pointer;
		if (kind == CALLBACK_GETC)
			result = &ffi_type_sint;
		if (kind == CALLBACK_COMPLETE || kind == CALLBACK_GENERATE) {
			count = kind == CALLBACK_COMPLETE ? 3u : 2u;
			binding->arguments[1] = binding->arguments[2] = &ffi_type_sint;
			result = &ffi_type_pointer;
		}
	}
	binding->closure = ffi_closure_alloc(sizeof(*binding->closure), &binding->code);
	if (!binding->closure ||
	    ffi_prep_cif(&binding->cif, FFI_DEFAULT_ABI, count, result, binding->arguments) !=
		    FFI_OK ||
	    ffi_prep_closure_loc(binding->closure, &binding->cif, dispatch, binding,
				 binding->code) != FFI_OK) {
		if (binding->closure)
			ffi_closure_free(binding->closure);
		free(binding);
		ctx->callback_error = -ENOMEM;
		return NULL;
	}
	binding->next = ctx->callbacks;
	ctx->callbacks = binding;
	return binding->code;
}

#define DEFINE_BINDING(name, member, kind, return_type, original_args, bound_args)                 \
	return_type(*cli_callback_##name(struct cli_context *ctx,                                  \
					 return_type(*callback) bound_args)) original_args         \
	{                                                                                          \
		union callback_target target = {0};                                                \
		return_type(*function) original_args;                                              \
		target.member = callback;                                                          \
		void *code = bind_callback(ctx, kind, target);                                     \
		_Static_assert(sizeof(function) == sizeof(code), "incompatible callback ABI");     \
		memcpy(&function, &code, sizeof(function));                                        \
		return function;                                                                   \
	}

DEFINE_BINDING(key, key, CALLBACK_KEY, int, (int, int), (struct cli_context *, int, int))
DEFINE_BINDING(display, display, CALLBACK_DISPLAY, void, (void), (struct cli_context *))
DEFINE_BINDING(line, line, CALLBACK_LINE, void, (char *), (struct cli_context *, char *))
DEFINE_BINDING(getc, getc, CALLBACK_GETC, int, (FILE *), (struct cli_context *, FILE *))
DEFINE_BINDING(complete, complete, CALLBACK_COMPLETE, char **, (const char *, int, int),
	       (struct cli_context *, const char *, int, int))
DEFINE_BINDING(generate, generate, CALLBACK_GENERATE, char *, (const char *, int),
	       (struct cli_context *, const char *, int))

void cli_callbacks_cleanup(struct cli_context *ctx)
{
	if (!ctx)
		return;
	while (ctx->callbacks) {
		struct cli_callback *binding = ctx->callbacks;

		ctx->callbacks = binding->next;
		ffi_closure_free(binding->closure);
		free(binding);
	}
	ctx->callback_error = 0;
}

/* Preserve stdio's buffered input while waiting in bounded cancellation slices. */
char *cli_input_fgets_until(struct cli_context *ctx, char *buffer, size_t capacity,
			    FILE *stream, int64_t *deadline)
{
	int fd = fileno(stream);
	int flags = fcntl(fd, F_GETFL);
	int error = 0;
	size_t length = 0;

	if (capacity < 2 || flags < 0) {
		errno = capacity < 2 ? EINVAL : errno;
		return NULL;
	}
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return NULL;
	while (length + 1 < capacity) {
		if (ctx && (morph_signal_flag_get(&ctx->interrupted) ||
			    morph_signal_flag_get(&ctx->terminate_requested) ||
			    morph_cancel_token_is_cancelled(ctx->input_cancel_token))) {
			error = EINTR;
			break;
		}
		errno = 0;
		int ch = fgetc(stream);

		if (ch != EOF) {
			if (deadline)
				*deadline = 0;
			buffer[length++] = (char)ch;
			if (ch == '\n')
				break;
			continue;
		}
		if (feof(stream))
			break;
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			error = errno ? errno : EIO;
			break;
		}
		clearerr(stream);
		if (deadline && *deadline > 0 && morph_io_now_ms() >= *deadline) {
			error = ETIMEDOUT;
			break;
		}
		struct pollfd descriptor = {.fd = fd, .events = POLLIN};

		if (poll(&descriptor, 1, 100) < 0 && errno != EINTR) {
			error = errno;
			break;
		}
	}
	(void)fcntl(fd, F_SETFL, flags);
	buffer[length] = '\0';
	errno = error;
	return !error && length ? buffer : NULL;
}

char *cli_input_fgets(struct cli_context *ctx, char *buffer, size_t capacity, FILE *stream)
{
	return cli_input_fgets_until(ctx, buffer, capacity, stream, NULL);
}

#ifdef HAVE_READLINE
void cli_keymap_free(Keymap map)
{
	if (map) {
		rl_discard_keymap(map);
		free(map);
	}
}

/* Readline's rl_copy_keymap shares submaps, including static library tables.
 * Clone each path so bindings and their cleanup belong entirely to this run. */
static Keymap copy_keymap(Keymap source, Keymap *ancestors, size_t depth)
{
	Keymap map;

	if (depth >= 64)
		return NULL;
	for (size_t i = 0; i < depth; i++) {
		if (source == ancestors[i])
			return NULL;
	}
	ancestors[depth] = source;
	map = rl_make_bare_keymap();
	if (!map)
		return NULL;
	for (int i = 0; i < KEYMAP_SIZE; i++) {
		if (source[i].type == ISKMAP) {
			Keymap child;

			_Static_assert(sizeof(child) == sizeof(source[i].function),
				       "incompatible keymap ABI");
			memcpy(&child, &source[i].function, sizeof(child));
			child = copy_keymap(child, ancestors, depth + 1);
			if (!child) {
				cli_keymap_free(map);
				return NULL;
			}
			map[i].type = ISKMAP;
			memcpy(&map[i].function, &child, sizeof(child));
		} else if (source[i].type == ISMACR) {
			char *text;

			memcpy(&text, &source[i].function, sizeof(text));
			text = text ? strdup(text) : NULL;
			if (!text && source[i].function) {
				cli_keymap_free(map);
				return NULL;
			}
			map[i].type = ISMACR;
			memcpy(&map[i].function, &text, sizeof(text));
		} else {
			map[i] = source[i];
		}
	}
	return map;
}

Keymap cli_keymap_copy(Keymap source)
{
	Keymap ancestors[64];

	return source ? copy_keymap(source, ancestors, 0) : NULL;
}

/* macOS poll reports POLLNVAL for a valid /dev/tty descriptor. Approval and
 * question prompts use that device, so wait with select for both input paths. */
static int cli_readline_wait(int fd, int milliseconds)
{
	fd_set input;
	struct timeval timeout = {
		.tv_sec = milliseconds / 1000,
		.tv_usec = (milliseconds % 1000) * 1000,
	};

	if (fd < 0 || fd >= FD_SETSIZE) {
		errno = EINVAL;
		return -EINVAL;
	}
	FD_ZERO(&input);
	FD_SET(fd, &input);
	int rc = select(fd + 1, &input, NULL, NULL, &timeout);

	return rc < 0 ? morph_error_from_errno(errno) : rc;
}

int cli_poll_getc(struct cli_context *ctx, FILE *stream)
{
	int fd = fileno(stream);

	while (!ctx || (!morph_signal_flag_get(&ctx->interrupted) &&
		       !morph_cancel_token_is_cancelled(ctx->input_cancel_token))) {
		int ready = cli_readline_wait(fd, 100);

		if (ready > 0) {
			if (ctx)
				ctx->input_deadline_ms = 0;
			int ch = rl_getc(stream);

			int suffix = 1;
			if (ch == 0x1b) {
				do {
					suffix = cli_readline_wait(fd, 30);
				} while (suffix < 0 && errno == EINTR);
			}
			if (ch == 0x1b && suffix == 0) {
				/* Consume a lone Esc even when idle, so Readline never
				 * waits for a prefix suffix and swallows the next edit. */
				if (ctx && ctx->turn_active) {
					if (!RL_ISSTATE(RL_STATE_CALLBACK)) {
						cli_request_cancel(ctx);
						rl_done = 1;
						return EOF;
					}
					runtime_cancel_turn(ctx->runtime);
				}
				return 0;
			}
			return ch;
		}
		if (ready < 0 && errno != EINTR)
			return EOF;
		if (ctx && ctx->input_deadline_ms > 0 &&
		    morph_io_now_ms() >= ctx->input_deadline_ms) {
			ctx->input_timed_out = 1;
			rl_done = 1;
			return EOF;
		}
	}
	rl_done = 1;
	return EOF;
}

char *cli_readline_prompt(struct cli_context *ctx, const char *prompt)
{
	int (*previous)(FILE *) = rl_getc_function;
	int (*reader)(FILE *) = ctx ? cli_callback_getc(ctx, cli_poll_getc) : rl_getc;
	int previous_catch_signals = rl_catch_signals;
	int previous_catch_winch = rl_catch_sigwinch;
	char *line;

	if (!reader)
		return NULL;
	rl_getc_function = reader;
	rl_catch_signals = 0;
	rl_catch_sigwinch = 0;
	line = readline(prompt);
	rl_getc_function = previous;
	rl_catch_signals = previous_catch_signals;
	rl_catch_sigwinch = previous_catch_winch;
	return line;
}
#endif
