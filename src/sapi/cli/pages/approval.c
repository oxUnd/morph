#include "sapi/cli/pages/approval.h"
#include "sapi/cli/presentation/terminal_text.h"
#include <nb/cleanup.h>
#include <nb/utf8.h>
#include <nb/err.h>
#include "sapi/cli/pages/chat.h"
#include "sapi/cli/application.h"
#include "sapi/cli/presentation/style.h"
#include <nothing/tui/terminal/size.h>
#include <nothing/tui/terminal.h>
#include "sapi/cli/adapters/input/state.h"
#include "agent/tool_context.h"
#include "cJSON.h"
#include "sapi/cli/adapters/input/reader.h"
#include "sapi/cli/adapters/input/cancel.h"
#include "sapi/cli/presentation/presenter.h"
#include "sapi/cli/presentation/output.h"
#include "sapi/cli/services/event_queue.h"
#include "sapi/cli/application_internal.h"
#include "sapi/cli/adapters/protocol/interaction.h"
#include "sapi/cli/pages/choice_dialog.h"
#include <sys/ioctl.h>

/* Preserve the line-oriented fallback for terminals without selector support. */
static int cli_approval_read_choice(struct cli_application *ctx, int scoped, int ephemeral)
{
	int v;

	const char *approval_prompt =
		scoped ? (ephemeral ? "  [y]es once / [s]ession / [n]o: "
				    : "  [y]es once / [s]ession / [a]lways / [n]o: ")
		       : "  [y]es / [n]o / [a]lways: ";
	char *rl_input =
		cli_readline_prompt_copy(ctx, cli_output_is_structured(ctx) ? "" : approval_prompt);
	if (!rl_input) {
		cli_output_printf(ctx, "\n");
		return NB_ERR_CANCELLED;
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
	return v;
}

/* Both approval flows share decision presentation and scope receipts. */
static int cli_approval_prompt_approval(struct cli_application *ctx, const char *subject,
					int scoped, int ephemeral)
{
	if (ctx->input->prompt_lock_ready)
		nb_mutex_lock(ctx->input->prompt_lock);

	int v = cli_approval_read_choice(ctx, scoped, ephemeral);

	if (v < 0) {
		if (ctx->input->prompt_lock_ready)
			nb_mutex_unlock(ctx->input->prompt_lock);

		return 0;
	}

	if (v > 0) {
		cli_output_printf(ctx, ANSI_GREEN CLI_FEED_DONE " Approved" ANSI_RESET " (");
		(void)cli_output_print_untrusted(ctx, subject ? subject : "",
						 MORPH_TERMINAL_TEXT_SINGLE_LINE);
		cli_output_printf(ctx, "%s)\n",
		       scoped && v == 2 ? ", session" : (v == (scoped ? 3 : 2) ? ", always" : ""));
	} else {
		cli_output_printf(ctx, ANSI_DIM CLI_FEED_CANCELLED " Denied" ANSI_RESET " (");
		(void)cli_output_print_untrusted(ctx, subject ? subject : "",
						 MORPH_TERMINAL_TEXT_SINGLE_LINE);
		cli_output_printf(ctx, ")\n");
	}

	fflush(stdout);
	if (ctx->input->prompt_lock_ready)
		nb_mutex_unlock(ctx->input->prompt_lock);

	return v;
}

static enum hitl_verdict cli_approval_json_hitl_approval(struct cli_application *ctx,
							 const char *tool_name,
							 const char *tool_args)
{
	static const char *const allowed[] = {"allow", "always", "deny"};
	cJSON *request = cJSON_CreateObject();
	char decision[16];
	int rc;

	if (!request)
		return HITL_DENY;

	if (!cJSON_AddStringToObject(request, "tool_name", tool_name ? tool_name : "") ||
	    !cJSON_AddStringToObject(request, "arguments_json", tool_args ? tool_args : "{}")) {
		cJSON_Delete(request);
		return HITL_DENY;
	}

	rc = cli_interaction_decision(ctx, "tool_approval", request, allowed,
				      (int)(sizeof(allowed) / sizeof(allowed[0])), decision,
				      sizeof(decision));
	cJSON_Delete(request);
	if (rc != 0 || strcmp(decision, "deny") == 0)
		return HITL_DENY;

	if (strcmp(decision, "always") == 0)
		return HITL_ALWAYS;

	return HITL_APPROVE;
}

struct cli_approval_hitl_call {
	const char *tool_name;
	const char *tool_args;
};

static int cli_approval_hitl_on_owner(struct cli_application *ctx, void *opaque)
{
	const struct cli_approval_hitl_call *call = opaque;

	return (int)cli_approval_tool_callback(call->tool_name, call->tool_args, ctx);
}

struct cli_approval_operation_call {
	const struct tool_operation *operation;
};

static int cli_approval_operation_on_owner(struct cli_application *ctx, void *opaque)
{
	const struct cli_approval_operation_call *call = opaque;

	return (int)cli_approval_operation_callback(call->operation, ctx);
}

/* Reserve a right gutter so terminal autowrap never owns card layout. */
static size_t cli_approval_card_width(void)
{
	int columns = nt_terminal_columns(STDOUT_FILENO, 80);

	return columns > 82 ? 80u : (columns >= 8 ? (size_t)columns - 2u : 0u);
}

static void cli_approval_card_rule(struct cli_application *ctx, size_t width, const char *title)
{
	size_t used = 1;

	cli_output_printf(ctx, ANSI_YELLOW "%s", title ? "╭" : "╰");
	if (title && width > 5) {
		const char *end = cli_terminal_text_advance(title, width - 5);

		cli_output_printf(ctx, "─ " ANSI_BOLD);
		cli_output_printf(ctx, "%.*s", (int)(end - title), title);
		cli_output_printf(ctx, ANSI_RESET ANSI_YELLOW " ");
		/* Approval titles contain only ASCII. */
		used = 4u + (size_t)(end - title);
	}

	for (; used < width - 1u; used++)
		cli_output_printf(ctx, "─");

	cli_output_printf(ctx, "%s" ANSI_RESET "\n", title ? "╮" : "╯");
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

	safe = morph_terminal_sanitize_dup(value, strlen(value), MORPH_TERMINAL_TEXT_SINGLE_LINE,
					   NULL);
	if (!safe)
		return;

	if (width == 0) {
		if (indent)
			cli_output_printf(ctx, "%-8s ", label);

		cli_output_printf(ctx, "%s%s" ANSI_RESET "\n", style, safe);
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
		char *end = (char *)cli_terminal_text_advance(line, budget);
		char saved = *end;

		*end = '\0';
		size_t used = indent + nb_utf8_width_cstr(line);

		cli_output_printf(ctx, ANSI_YELLOW "│ " ANSI_RESET);
		if (indent) {
			size_t label_width = first ? nb_utf8_width_cstr(label) : 0u;

			if (first)
				cli_output_printf(ctx, "%s", label);

			for (; label_width < indent; label_width++)
				cli_output_putchar(ctx, ' ');
		}

		cli_output_printf(ctx, "%s", style);
		cli_output_printf(ctx, "%s", line);
		*end = saved;
		cli_output_printf(ctx, ANSI_RESET);
		for (; used < width - 4u; used++)
			cli_output_putchar(ctx, ' ');

		cli_output_printf(ctx, ANSI_YELLOW " │" ANSI_RESET "\n");
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
		cli_output_printf(ctx, "\n");
		cli_approval_card_rule(ctx, width, CLI_FEED_QUESTION " Approval required");
	}

	if (!width)
		cli_output_printf(ctx, "approval: ");

	cli_approval_card_field(ctx, width, width ? "Tool" : NULL, tool_name, ANSI_BOLD);
	if (tool_args && *tool_args && strcmp(tool_args, "{}") != 0) {
		if (!width)
			cli_output_printf(ctx, "args: ");

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

	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE && !cli_event_queue_is_owner(ctx)) {
		int decision = cli_approval_request(ctx, NULL, tool_name, tool_args, NULL);
		return decision == 2 ? HITL_ALWAYS : decision == 1 ? HITL_APPROVE : HITL_DENY;
	}

	if (cli_chat_active(ctx))
		return HITL_DENY;

	if (ctx->events && !cli_event_queue_is_owner(ctx)) {
		struct cli_approval_hitl_call call = {tool_name, tool_args};

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

static int cli_approval_json_add_optional_string(cJSON *object, const char *name, const char *value)
{
	if (!value || !value[0])
		return 0;

	if (!cJSON_AddStringToObject(object, name, value))
		return NB_ERR_NOMEM;

	return 0;
}

static enum tool_operation_verdict
cli_approval_json_operation_approval(struct cli_application *ctx, const struct tool_operation *op)
{
	static const char *const persistent[] = {"allow", "session", "always", "deny"};
	static const char *const ephemeral_decisions[] = {"allow", "session", "deny"};
	const char *const *allowed = persistent;
	int allowed_count = (int)(sizeof(persistent) / sizeof(persistent[0]));
	cJSON *request;
	cJSON *programs;
	char decision[16] = {0};
	int ephemeral;
	int rc;

	request = cJSON_CreateObject();
	programs = cJSON_CreateArray();
	if (!request || !programs) {
		cJSON_Delete(request);
		cJSON_Delete(programs);
		return TOOL_OP_DENY;
	}

	ephemeral = op->tool_name && strcmp(op->tool_name, "exec") == 0 &&
		    (op->kind == TOOL_OP_PATH_WRITE || op->kind == TOOL_OP_PATH_DELETE);
	if (!cJSON_AddStringToObject(request, "operation",
				     cli_approval_operation_approval_subject(op)) ||
	    !cJSON_AddBoolToObject(request, "ephemeral", ephemeral) ||
	    cli_approval_json_add_optional_string(request, "access",
						  tool_path_access_name(op->path_access)) != 0 ||
	    cli_approval_json_add_optional_string(request, "tool_name", op->tool_name) != 0 ||
	    cli_approval_json_add_optional_string(request, "principal", op->principal) != 0 ||
	    cli_approval_json_add_optional_string(request, "action", op->action) != 0 ||
	    cli_approval_json_add_optional_string(request, "target", op->target) != 0 ||
	    cli_approval_json_add_optional_string(request, "scope", op->scope) != 0 ||
	    cli_approval_json_add_optional_string(request, "reason", op->reason) != 0 ||
	    cli_approval_json_add_optional_string(request, "details_json", op->details_json) != 0) {
		cJSON_Delete(programs);
		cJSON_Delete(request);
		return TOOL_OP_DENY;
	}

	for (int i = 0; i < op->programs_count; i++) {
		cJSON *program = cJSON_CreateString(op->programs[i]);

		if (!program) {
			cJSON_Delete(programs);
			cJSON_Delete(request);
			return TOOL_OP_DENY;
		}

		cJSON_AddItemToArray(programs, program);
	}

	cJSON_AddItemToObject(request, "programs", programs);
	if (ephemeral) {
		allowed = ephemeral_decisions;
		allowed_count = (int)(sizeof(ephemeral_decisions) / sizeof(ephemeral_decisions[0]));
	}

	rc = cli_interaction_decision(ctx, "operation_approval", request, allowed, allowed_count,
				      decision, sizeof(decision));
	cJSON_Delete(request);
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
		cli_output_printf(ctx, "\n");
		cli_approval_card_rule(ctx, width, cli_approval_operation_approval_label(op));
	} else {
		cli_output_printf(ctx, "approval: %s\n", cli_approval_operation_approval_label(op));
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
			nb_buf programs NB_CLEANUP(nb_buf_cleanup) = {0};

			if ((nb_buf_init(&programs),
			     nb_buf_reserve(&programs, 128) ? 0 : NB_ERR_NOMEM) == 0) {
				for (int i = 0; i < op->programs_count; i++) {
					if (i > 0)
						(void)nb_buf_append_cstr(&programs, ", ");

					(void)nb_buf_append_cstr(&programs, op->programs[i]);
				}

				cli_approval_card_field(ctx, width, "Programs",
							nb_buf_cstr(&programs), ANSI_BOLD);
				nb_buf_cleanup(&programs);
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

	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE && !cli_event_queue_is_owner(ctx)) {
		int decision = cli_approval_request(ctx, op, NULL, NULL, NULL);
		return decision == 3   ? TOOL_OP_ALWAYS
		       : decision == 2 ? TOOL_OP_SESSION
		       : decision == 1 ? TOOL_OP_ALLOW
				       : TOOL_OP_DENY;
	}

	if (cli_chat_active(ctx))
		return TOOL_OP_DENY;

	if (ctx->events && !cli_event_queue_is_owner(ctx)) {
		struct cli_approval_operation_call call = {.operation = op};

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

struct cli_approval_request {
	const struct tool_operation *operation;
	const char *tool_name, *tool_args;
	const struct nb_cancel *cancel;
};

struct cli_approval_pending {
	struct cli_approval_request *request;
	struct cli_event_queue_owner_call *completion;
	int verdicts[4];
	int count;
};

static void cli_approval_pending_cleanup(struct cli_approval_pending **owner)
{
	free(*owner);
	*owner = NULL;
}

static void cli_approval_pending_release(void *data)
{
	free(data);
}

static void cli_approval_native_complete(struct cli_application *ctx, int result, size_t index,
					 const char *answer, void *data)
{
	(void)answer;
	struct cli_approval_pending *pending = data;
	const struct tool_operation *op = pending->request->operation;
	int scoped = op != NULL;
	const char *subject =
		op ? cli_approval_operation_approval_subject(op) : pending->request->tool_name;
	int decision = !result && index < (size_t)pending->count ? pending->verdicts[index] : 0;
	if (decision) {
		cli_output_printf(ctx, ANSI_GREEN CLI_FEED_DONE " Approved" ANSI_RESET " (");
		(void)cli_output_print_untrusted(ctx, subject, MORPH_TERMINAL_TEXT_SINGLE_LINE);
		cli_output_printf(ctx, "%s)\n",
				  scoped && decision == 2	 ? ", session"
				  : decision == (scoped ? 3 : 2) ? ", always"
								 : "");
	} else {
		cli_output_printf(ctx, ANSI_DIM CLI_FEED_CANCELLED " Denied" ANSI_RESET " (");
		(void)cli_output_print_untrusted(ctx, subject, MORPH_TERMINAL_TEXT_SINGLE_LINE);
		cli_output_printf(ctx, ")\n");
	}

	cli_cancel_state_reset(ctx);
	cli_event_queue_complete_owner_call(pending->completion, decision);
}

static int cli_approval_native_start(struct cli_application *ctx, void *data,
				     struct cli_event_queue_owner_call *completion)
{
	struct cli_approval_request *request = data;
	if (!cli_chat_active(ctx))
		return NB_ERR_NOT_TERMINAL;

	struct cli_approval_pending *pending NB_CLEANUP(cli_approval_pending_cleanup) =
		calloc(1, sizeof(*pending));
	if (!pending)
		return NB_ERR_NOMEM;

	pending->request = request;
	pending->completion = completion;
	const struct tool_operation *op = request->operation;
	int ephemeral = op && op->tool_name && !strcmp(op->tool_name, "exec") &&
			(op->kind == TOOL_OP_PATH_WRITE || op->kind == TOOL_OP_PATH_DELETE);
	cli_cancel_monitor_clear(ctx);
	cli_presentation_prepare_prompt(ctx);
	if (op)
		cli_approval_render_operation(ctx, op);
	else
		cli_approval_render_tool(ctx, request->tool_name, request->tool_args);

	const char *items[4] = {"Yes once"};
	pending->verdicts[pending->count++] = 1;
	if (op) {
		items[pending->count] = "Allow for this session";
		pending->verdicts[pending->count++] = 2;
	}

	if (!ephemeral) {
		items[pending->count] = op ? "Always allow for this project" : "Always allow";
		pending->verdicts[pending->count++] = op ? 3 : 2;
	}

	items[pending->count] = "No";
	pending->verdicts[pending->count++] = 0;
	const struct nb_cancel *cancel =
		request->cancel ? request->cancel : ctx->input->input_cancel_token;
	int rc = cli_choice_dialog_show(
		ctx, &(struct cli_choice_dialog_options){.choices = items,
							 .count = pending->count,
							 .cancel = cancel,
							 .complete = cli_approval_native_complete,
							 .data = pending,
							 .release = cli_approval_pending_release});
	if (!rc) {
		pending = NULL;
		cli_chat_panel_set_interaction(ctx, CLI_CHAT_INTERACTION_APPROVAL);
	}

	return rc;
}

int cli_approval_request(struct cli_application *ctx, const struct tool_operation *operation,
			 const char *tool_name, const char *tool_args,
			 const struct nb_cancel *cancel)
{
	struct cli_approval_request request = {.operation = operation,
					       .tool_name = tool_name,
					       .tool_args = tool_args,
					       .cancel = cancel};
	int decision =
		cli_event_queue_call_owner_deferred(ctx, cli_approval_native_start, &request);
	return decision < 0 ? 0 : decision;
}
