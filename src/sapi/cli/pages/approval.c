#include "sapi/cli/pages/approval.h"
#include "util/terminal_size.h"
#include <morph/tui/terminal.h>
#include "sapi/cli/adapters/input/state.h"
#include "agent/tool_context.h"
#include "util/json.h"
#include "sapi/cli/adapters/input/reader.h"
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/adapters/protocol/interaction.h"
#include <morph/tui/terminal.h>
#include <morph/tui/widgets/select.h>
#include <morph/tui/widgets/tree.h>
#include <sys/ioctl.h>

static int cli_approval_select_choice(struct cli_application *ctx, int scoped, int ephemeral)
{
	FILE *tty = NULL;
	FILE *input = stdin;
	const char *items[4];
	int verdicts[4];
	size_t count = 0;
	int verdict = 0;

	if (!isatty(STDOUT_FILENO))
		return -ENOTTY;
	if (!isatty(STDIN_FILENO)) {
		/* Open the actual device: macOS poll rejects the /dev/tty alias. */
		const char *path = ttyname(STDOUT_FILENO);

		tty = path ? fopen(path, "r") : NULL;
		input = tty;
	}
	if (!morph_tui_terminal_available(input, stdout)) {
		if (tty)
			fclose(tty);
		return -ENOTTY;
	}
	items[count] = "Yes once";
	verdicts[count++] = 1;
	if (scoped) {
		items[count] = "Allow for this session";
		verdicts[count++] = 2;
	}
	if (!ephemeral) {
		items[count] = scoped ? "Always allow for this project" : "Always allow";
		verdicts[count++] = scoped ? 3 : 2;
	}
	items[count] = "No";
	verdicts[count++] = 0;
	struct morph_tui_widget_select state = {0};
	const struct morph_tui_widget_select_options options = {
		.items = items,
		.interrupted = &ctx->interrupted,
		.color = cli_output_color_enabled(ctx),
	};
	int rc = morph_tui_widget_select_init(&state, count, 0, 1, 1);

	if (!rc) {
		printf(ctx, "\n");
		fflush(stdout);
		rc = morph_tui_widget_select_run(&state, &options, input, stdout, NULL);
	}
	if (!rc)
		verdict = verdicts[state.menu.list.index];
	morph_tui_widget_select_cleanup(&state);
	if (tty)
		fclose(tty);
	return verdict;
}

/* Preserve the line-oriented fallback for terminals without selector support. */
static int cli_approval_read_choice(struct cli_application *ctx, int scoped, int ephemeral)
{
	int v;

#ifdef HAVE_READLINE
	const char *approval_prompt =
		scoped ? (ephemeral ? "  [y]es once / [s]ession / [n]o: "
				    : "  [y]es once / [s]ession / [a]lways / [n]o: ")
		       : "  [y]es / [n]o / [a]lways: ";
	char *rl_input =
		cli_readline_prompt(ctx, cli_output_is_structured(ctx) ? "" : approval_prompt);
	if (!rl_input) {
		printf(ctx, "\n");
		return -ECANCELED;
	}
	if (!ephemeral && (rl_input[0] == 'a' || rl_input[0] == 'A'))
		v = scoped ? 3 : 2;
	else if (scoped && (rl_input[0] == 's' || rl_input[0] == 'S'))
		v = 2;
	else if (rl_input[0] == 'y' || rl_input[0] == 'Y')
		v = 1;
	else
		v = 0;
	free(rl_input);
#else
	if (scoped && ephemeral)
		printf(ctx, "  [" ANSI_GREEN "y" ANSI_RESET "]es once / "
			    "[s]ession / [" ANSI_RED "n" ANSI_RESET "]o: ");
	else if (scoped)
		printf(ctx, "  [" ANSI_GREEN "y" ANSI_RESET "]es once / "
			    "[s]ession / [a]lways / [" ANSI_RED "n" ANSI_RESET "]o: ");
	else
		printf(ctx, "  [" ANSI_GREEN "y" ANSI_RESET "]es / [" ANSI_RED "n" ANSI_RESET
			    "]o / [a]lways: ");
	fflush(stdout);

	char buf[16];
	FILE *tty = fopen("/dev/tty", "r");
	if (!tty) {
		printf(ctx, "\n");
		return -ECANCELED;
	}
	if (!cli_input_fgets(ctx, buf, sizeof(buf), tty)) {
		fclose(tty);
		printf(ctx, "\n");
		return -ECANCELED;
	}
	fclose(tty);

	if (!ephemeral && (buf[0] == 'a' || buf[0] == 'A'))
		v = scoped ? 3 : 2;
	else if (scoped && (buf[0] == 's' || buf[0] == 'S'))
		v = 2;
	else if (buf[0] == 'y' || buf[0] == 'Y')
		v = 1;
	else
		v = 0;
#endif
	return v;
}

/* Both approval flows share decision presentation and scope receipts. */
static int cli_approval_prompt_approval(struct cli_application *ctx, const char *subject,
					int scoped, int ephemeral)
{
	if (ctx->input->prompt_lock_ready)
		pthread_mutex_lock(&ctx->input->prompt_lock);
	int v = cli_approval_select_choice(ctx, scoped, ephemeral);

	if (v == -ENOTTY)
		v = cli_approval_read_choice(ctx, scoped, ephemeral);
	if (v < 0) {
		if (ctx->input->prompt_lock_ready)
			pthread_mutex_unlock(&ctx->input->prompt_lock);
		return 0;
	}

	if (v > 0) {
		printf(ctx, ANSI_GREEN CLI_FEED_DONE " Approved" ANSI_RESET " (");
		(void)cli_output_print_untrusted(ctx, subject ? subject : "",
						 UTF8_TERMINAL_TEXT_SINGLE_LINE);
		printf(ctx, "%s)\n",
		       scoped && v == 2 ? ", session" : (v == (scoped ? 3 : 2) ? ", always" : ""));
	} else {
		printf(ctx, ANSI_RED CLI_FEED_SYSTEM " Denied" ANSI_RESET " (");
		(void)cli_output_print_untrusted(ctx, subject ? subject : "",
						 UTF8_TERMINAL_TEXT_SINGLE_LINE);
		printf(ctx, ")\n");
	}

	fflush(stdout);
	if (ctx->input->prompt_lock_ready)
		pthread_mutex_unlock(&ctx->input->prompt_lock);
	return v;
}

static enum hitl_verdict cli_approval_json_hitl_approval(struct cli_application *ctx,
							 const char *tool_name,
							 const char *tool_args)
{
	static const char *const allowed[] = {"allow", "always", "deny"};
	struct json *request = json_object();
	char decision[16];
	int rc;

	if (!request)
		return HITL_DENY;
	if (!json_add_string(request, "tool_name", tool_name ? tool_name : "") ||
	    !json_add_string(request, "arguments_json", tool_args ? tool_args : "{}")) {
		json_free(request);
		return HITL_DENY;
	}
	rc = cli_interaction_decision(ctx, "tool_approval", request, allowed,
				      (int)(sizeof(allowed) / sizeof(allowed[0])), decision,
				      sizeof(decision));
	json_free(request);
	if (rc != 0 || strcmp(decision, "deny") == 0)
		return HITL_DENY;
	if (strcmp(decision, "always") == 0)
		return HITL_ALWAYS;
	return HITL_APPROVE;
}

struct cli_approval_hitl_call {
	struct cli_application *ctx;
	const char *tool_name;
	const char *tool_args;
};

static int cli_approval_hitl_on_owner(void *opaque)
{
	struct cli_approval_hitl_call *call = opaque;
	struct cli_application *ctx = call->ctx;
	(void)ctx;

	return (int)cli_approval_tool_callback(call->tool_name, call->tool_args, call->ctx);
}

struct cli_approval_operation_call {
	struct cli_application *ctx;
	const struct tool_operation *operation;
};

static int cli_approval_operation_on_owner(void *opaque)
{
	struct cli_approval_operation_call *call = opaque;
	struct cli_application *ctx = call->ctx;
	(void)ctx;

	return (int)cli_approval_operation_callback(call->operation, call->ctx);
}

/* Reserve a right gutter so terminal autowrap never owns card layout. */
static size_t cli_approval_card_width(void)
{
	int columns = morph_terminal_columns(STDOUT_FILENO, 80);

	return columns > 82 ? 80u : (columns >= 8 ? (size_t)columns - 2u : 0u);
}

static void cli_approval_card_rule(struct cli_application *ctx, size_t width, const char *title)
{
	size_t used = 1;

	printf(ctx, ANSI_YELLOW "%s", title ? "╭" : "╰");
	if (title && width > 5) {
		const char *end = utf8_advance_display_width(title, width - 5);

		printf(ctx, "─ " ANSI_BOLD);
		fwrite(title, 1, (size_t)(end - title), stdout);
		printf(ctx, ANSI_RESET ANSI_YELLOW " ");
		/* Approval titles contain only ASCII. */
		used = 4u + (size_t)(end - title);
	}
	for (; used < width - 1u; used++)
		printf(ctx, "─");
	printf(ctx, "%s" ANSI_RESET "\n", title ? "╮" : "╯");
}

static void cli_approval_card_field(struct cli_application *ctx, size_t width, const char *label,
				    const char *value, const char *style)
{
	char *safe;
	char *line;
	size_t indent = label && *label ? 11u : 0u;
	int first = 1;

	if (!value)
		value = "";
	safe = utf8_terminal_sanitize_dup(value, strlen(value), UTF8_TERMINAL_TEXT_SINGLE_LINE,
					  NULL);
	if (!safe)
		return;
	if (width == 0) {
		if (indent)
			printf(ctx, "%-8s ", label);
		printf(ctx, "%s%s" ANSI_RESET "\n", style, safe);
		free(safe);
		return;
	}
	if (indent && width < indent + 6u) {
		cli_approval_card_field(ctx, width, NULL, label, style);
		indent = 0;
	}
	line = safe;
	do {
		size_t budget = width - 4u - indent;
		char *end = (char *)utf8_advance_display_width(line, budget);
		char saved = *end;

		*end = '\0';
		size_t used = indent + utf8_display_width(line);

		printf(ctx, ANSI_YELLOW "│ " ANSI_RESET);
		if (indent) {
			size_t label_width = first ? utf8_display_width(label) : 0u;

			if (first)
				printf(ctx, "%s", label);
			for (; label_width < indent; label_width++)
				putchar(ctx, ' ');
		}
		printf(ctx, "%s", style);
		fwrite(line, 1, (size_t)(end - line), stdout);
		*end = saved;
		printf(ctx, ANSI_RESET);
		for (; used < width - 4u; used++)
			putchar(ctx, ' ');
		printf(ctx, ANSI_YELLOW " │" ANSI_RESET "\n");
		line = end;
		first = 0;
	} while (*line);
	free(safe);
}

void cli_approval_render_tool(struct cli_application *ctx, const char *tool_name,
			      const char *tool_args)
{
	size_t width =
		ctx->presentation_mode == CLI_PRESENT_INTERACTIVE ? cli_approval_card_width() : 0u;

	if (width) {
		printf(ctx, "\n");
		cli_approval_card_rule(ctx, width, CLI_FEED_QUESTION " Approval required");
	}
	if (!width)
		printf(ctx, "approval: ");
	cli_approval_card_field(ctx, width, width ? "Tool" : NULL, tool_name, ANSI_BOLD);
	if (tool_args && *tool_args && strcmp(tool_args, "{}") != 0) {
		if (!width)
			printf(ctx, "args: ");
		cli_approval_card_field(ctx, width, width ? "Args" : NULL, tool_args, ANSI_DIM);
	}
	if (width)
		cli_approval_card_rule(ctx, width, NULL);
}

enum hitl_verdict cli_approval_tool_callback(const char *tool_name, const char *tool_args,
					     void *user_data)
{
	struct cli_application *ctx = user_data;
	if (!ctx)
		return HITL_DENY;
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
		return cli_approval_json_hitl_approval(ctx, tool_name, tool_args);
	if (ctx->events && !cli_event_queue_is_owner(ctx)) {
		struct cli_approval_hitl_call call = {ctx, tool_name, tool_args};

		return (enum hitl_verdict)cli_event_queue_call_owner(
			ctx, cli_approval_hitl_on_owner, &call);
	}

	cli_cancel_monitor_clear(ctx);
	cli_presentation_prepare_prompt(ctx);
	cli_approval_render_tool(ctx, tool_name, tool_args);

	int v = cli_approval_prompt_approval(ctx, tool_name, 0, 0);
	if (v == 2)
		return HITL_ALWAYS;
	if (v == 1)
		return HITL_APPROVE;
	return HITL_DENY;
}

static const char *cli_approval_operation_label(enum tool_operation_kind kind)
{
	switch (kind) {
	case TOOL_OP_COMMAND:
		return "Command Approval Required";
	case TOOL_OP_PATH_READ:
		return "Read Path Approval";
	case TOOL_OP_PATH_LIST:
		return "List Path Approval";
	case TOOL_OP_PATH_WRITE:
		return "Write Path Approval";
	case TOOL_OP_PATH_DELETE:
		return "Delete Path Approval";
	case TOOL_OP_DIRECTORY_ACCESS:
		return "Directory Access Approval";
	case TOOL_OP_NETWORK:
		return "Network Approval";
	case TOOL_OP_EXTERNAL_SEND:
		return "External Send Approval";
	case TOOL_OP_TOOL_CALL:
		return "Tool Approval";
	}
	return "Operation Approval";
}

static const char *cli_approval_operation_approval_label(const struct tool_operation *op)
{
	if (op->path_access && (op->path_access & (op->path_access - 1u)))
		return "Directory Access Approval";
	return cli_approval_operation_label(op->kind);
}

static const char *cli_approval_operation_subject(enum tool_operation_kind kind)
{
	switch (kind) {
	case TOOL_OP_COMMAND:
		return "command";
	case TOOL_OP_PATH_READ:
		return "read_path";
	case TOOL_OP_PATH_LIST:
		return "list_path";
	case TOOL_OP_PATH_WRITE:
		return "write_path";
	case TOOL_OP_PATH_DELETE:
		return "delete_path";
	case TOOL_OP_DIRECTORY_ACCESS:
		return "directory_access";
	case TOOL_OP_NETWORK:
		return "network";
	case TOOL_OP_EXTERNAL_SEND:
		return "external_send";
	case TOOL_OP_TOOL_CALL:
		return "tool_call";
	}
	return "operation";
}

static const char *cli_approval_operation_approval_subject(const struct tool_operation *op)
{
	if (op->path_access && (op->path_access & (op->path_access - 1u)))
		return "directory_access";
	return cli_approval_operation_subject(op->kind);
}

static const char *cli_approval_operation_scope_label(enum tool_operation_kind kind)
{
	switch (kind) {
	case TOOL_OP_COMMAND:
		return "Workdir";
	case TOOL_OP_PATH_READ:
	case TOOL_OP_PATH_LIST:
		return "Workspace";
	case TOOL_OP_PATH_WRITE:
	case TOOL_OP_PATH_DELETE:
		return "Output dir";
	case TOOL_OP_DIRECTORY_ACCESS:
		return "Lifetime";
	case TOOL_OP_NETWORK:
		return "Scope";
	case TOOL_OP_EXTERNAL_SEND:
	case TOOL_OP_TOOL_CALL:
		return "Scope";
	}
	return "Scope";
}

static int cli_approval_json_add_optional_string(struct json *object, const char *name,
						 const char *value)
{
	if (!value || !value[0])
		return 0;
	if (!json_add_string(object, name, value))
		return -ENOMEM;
	return 0;
}

static enum tool_operation_verdict
cli_approval_json_operation_approval(struct cli_application *ctx, const struct tool_operation *op)
{
	static const char *const persistent[] = {"allow", "session", "always", "deny"};
	static const char *const ephemeral_decisions[] = {"allow", "session", "deny"};
	const char *const *allowed = persistent;
	int allowed_count = (int)(sizeof(persistent) / sizeof(persistent[0]));
	struct json *request;
	struct json *programs;
	char decision[16] = {0};
	int ephemeral;
	int rc;

	request = json_object();
	programs = json_array();
	if (!request || !programs) {
		json_free(request);
		json_free(programs);
		return TOOL_OP_DENY;
	}
	ephemeral = op->tool_name && strcmp(op->tool_name, "exec") == 0 &&
		    (op->kind == TOOL_OP_PATH_WRITE || op->kind == TOOL_OP_PATH_DELETE);
	if (!json_add_string(request, "operation", cli_approval_operation_approval_subject(op)) ||
	    !json_add_bool(request, "ephemeral", ephemeral) ||
	    cli_approval_json_add_optional_string(request, "access",
						  tool_path_access_name(op->path_access)) != 0 ||
	    cli_approval_json_add_optional_string(request, "tool_name", op->tool_name) != 0 ||
	    cli_approval_json_add_optional_string(request, "principal", op->principal) != 0 ||
	    cli_approval_json_add_optional_string(request, "action", op->action) != 0 ||
	    cli_approval_json_add_optional_string(request, "target", op->target) != 0 ||
	    cli_approval_json_add_optional_string(request, "scope", op->scope) != 0 ||
	    cli_approval_json_add_optional_string(request, "reason", op->reason) != 0 ||
	    cli_approval_json_add_optional_string(request, "details_json", op->details_json) != 0) {
		json_free(programs);
		json_free(request);
		return TOOL_OP_DENY;
	}
	for (int i = 0; i < op->programs_count; i++) {
		struct json *program = json_string(op->programs[i]);

		if (!program) {
			json_free(programs);
			json_free(request);
			return TOOL_OP_DENY;
		}
		if (json_array_add(programs, program)) {
			json_free(program);
			json_free(programs);
			json_free(request);
			return TOOL_OP_DENY;
		}
	}
	if (json_object_add(request, "programs", programs)) {
		json_free(programs);
		json_free(request);
		return TOOL_OP_DENY;
	}
	if (ephemeral) {
		allowed = ephemeral_decisions;
		allowed_count = (int)(sizeof(ephemeral_decisions) / sizeof(ephemeral_decisions[0]));
	}
	rc = cli_interaction_decision(ctx, "operation_approval", request, allowed, allowed_count,
				      decision, sizeof(decision));
	json_free(request);
	if (rc != 0 || strcmp(decision, "deny") == 0)
		return TOOL_OP_DENY;
	if (strcmp(decision, "always") == 0)
		return TOOL_OP_ALWAYS;
	if (strcmp(decision, "session") == 0)
		return TOOL_OP_SESSION;
	return TOOL_OP_ALLOW;
}

void cli_approval_render_operation(struct cli_application *ctx, const struct tool_operation *op)
{
	size_t width =
		ctx->presentation_mode == CLI_PRESENT_INTERACTIVE ? cli_approval_card_width() : 0u;
	int ephemeral = op->tool_name && strcmp(op->tool_name, "exec") == 0 &&
			(op->kind == TOOL_OP_PATH_WRITE || op->kind == TOOL_OP_PATH_DELETE);

	if (width) {
		printf(ctx, "\n");
		cli_approval_card_rule(ctx, width, cli_approval_operation_approval_label(op));
	} else {
		printf(ctx, "approval: %s\n", cli_approval_operation_approval_label(op));
	}
	if (op->tool_name && *op->tool_name)
		cli_approval_card_field(ctx, width, "Tool", op->tool_name, ANSI_BOLD);
	if (op->kind != TOOL_OP_COMMAND && op->principal && *op->principal &&
	    strcmp(op->principal, "*") != 0)
		cli_approval_card_field(ctx, width, "Subject", op->principal, ANSI_BOLD);
	if (op->kind != TOOL_OP_COMMAND && op->principal && strcmp(op->principal, "*") == 0)
		cli_approval_card_field(ctx, width, "Applies to", "All tools and commands",
					ANSI_BOLD);
	if (op->reason && *op->reason)
		cli_approval_card_field(ctx, width, "Reason", op->reason, ANSI_BOLD);
	if (op->kind == TOOL_OP_COMMAND) {
		if (op->action)
			cli_approval_card_field(ctx, width, "Command", op->action, ANSI_BOLD);
		if (op->programs_count > 0) {
			morph_buf_t programs;

			if (morph_buf_init(&programs, 128) == 0) {
				for (int i = 0; i < op->programs_count; i++) {
					if (i > 0)
						(void)morph_buf_puts(&programs, ", ");
					(void)morph_buf_puts(&programs, op->programs[i]);
				}
				cli_approval_card_field(ctx, width, "Programs",
							morph_buf_cstr(&programs), ANSI_BOLD);
				morph_buf_cleanup(&programs);
			}
		}
	} else if (op->target && *op->target) {
		cli_approval_card_field(ctx, width, "Target", op->target, ANSI_BOLD);
	}
	if (op->path_access)
		cli_approval_card_field(ctx, width, "Access",
					tool_path_access_name(op->path_access), ANSI_BOLD);
	if (op->scope && *op->scope)
		cli_approval_card_field(
			ctx, width,
			op->tool_name && strcmp(op->tool_name, "request_permissions") == 0
				? "Lifetime"
				: cli_approval_operation_scope_label(op->kind),
			op->scope, ANSI_DIM);
	if (op->kind == TOOL_OP_COMMAND)
		cli_approval_card_field(ctx, width, NULL,
					"'yes once' runs only the command shown; "
					"'session' trusts the listed programs and workdir "
					"until exit; 'always' remembers that trust for this "
					"project.",
					ANSI_DIM);
	else if (ephemeral)
		cli_approval_card_field(ctx, width, NULL,
					"'yes' allows this call; 'session' trusts "
					"this capability until exit. It is never persisted.",
					ANSI_DIM);
	else if (op->path_access)
		cli_approval_card_field(ctx, width, NULL,
					"Approves the listed access. 'session': until exit. "
					"'always': remember for this project.",
					ANSI_DIM);
	else
		cli_approval_card_field(ctx, width, NULL,
					"'session' trusts this scope until exit; "
					"'always' remembers it for this project.",
					ANSI_DIM);
	if (width)
		cli_approval_card_rule(ctx, width, NULL);
}

enum tool_operation_verdict cli_approval_operation_callback(const struct tool_operation *op,
							    void *user_data)
{
	struct cli_application *ctx = user_data;
	if (!ctx || !op)
		return TOOL_OP_DENY;
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
		return cli_approval_json_operation_approval(ctx, op);
	if (ctx->events && !cli_event_queue_is_owner(ctx)) {
		struct cli_approval_operation_call call = {ctx, op};

		return (enum tool_operation_verdict)cli_event_queue_call_owner(
			ctx, cli_approval_operation_on_owner, &call);
	}

	cli_cancel_monitor_clear(ctx);
	cli_presentation_prepare_prompt(ctx);
	cli_approval_render_operation(ctx, op);
	int ephemeral = op->tool_name && strcmp(op->tool_name, "exec") == 0 &&
			(op->kind == TOOL_OP_PATH_WRITE || op->kind == TOOL_OP_PATH_DELETE);

	int v = cli_approval_prompt_approval(ctx, cli_approval_operation_approval_subject(op), 1,
					     ephemeral);
	if (v == 3)
		return TOOL_OP_ALWAYS;
	if (v == 2)
		return TOOL_OP_SESSION;
	if (v == 1)
		return TOOL_OP_ALLOW;
	return TOOL_OP_DENY;
}
