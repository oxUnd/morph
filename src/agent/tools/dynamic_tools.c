#include "dynamic_tools.h"
#include "util/cleanup.h"
#include "dynamic_tool_service.h"
#include "dynamic_tool_internal.h"
#include "util/error.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static int tool_create_exec(const char *args, struct tool_result *result, void *user_data);
static int tool_promote_exec(const char *args, struct tool_result *result, void *user_data);
static int tool_delete_exec(const char *args, struct tool_result *result, void *user_data);
static int tool_history_exec(const char *args, struct tool_result *result, void *user_data);
static int tool_diff_exec(const char *args, struct tool_result *result, void *user_data);
static int tool_rollback_exec(const char *args, struct tool_result *result, void *user_data);
static void dynamic_tools_context_destroy(void *user_data);

static const char TOOL_CREATE_DESCRIPTION[] =
	"Create or update a dynamic JavaScript tool for this agent session. "
	"The source_js runs in embedded QuickJS. It must define global function "
	"run(args) or async function run(args), and return a JSON-serializable "
	"value. This is not Node.js: no node: imports, fs/http/https/"
	"child_process modules, process, Buffer, __dirname, require(), or npm "
	"packages. Use only the bounded morph host APIs. Image APIs: "
	"morph.image.metadata({input}), morph.image.resize({input,output,width,"
	"height}), morph.image.crop/extract({input,output,left,top,width,"
	"height}), morph.image.extend({input,output,top,bottom,left,right,"
	"background}), morph.image.rotate({input,output,angle}), "
	"morph.image.compose({input,output,overlays:[{input,left,top}]}), "
	"morph.image.convert({input,output}), morph.image.frame({input,output,"
	"style,caption,padding}), and morph.image.open(input) for the same "
	"finite image handle methods: metadata, resize, extract, extend, "
	"rotate, blur, sharpen, grayscale/greyscale, flatten, composite, png, "
	"jpeg/jpg, webp, toFile, toBuffer. Canvas APIs: "
	"morph.canvas.create({width,height}), morph.canvas.loadImage({input}), "
	"morph.canvas.toFile({canvas,output}), morph.canvas.toBuffer({canvas}); "
	"canvas handles support getContext(\"2d\"), fillRect, strokeRect, "
	"rect, beginPath, fill, stroke, save, restore, moveTo, lineTo, "
	"translate, arc, fillText, strokeText, drawImage, toFile, toBuffer. "
	"Canvas text supports ctx.font, ctx.textAlign (left, right, center, "
	"start, end), and ctx.textBaseline (top, hanging, middle, alphabetic, "
	"ideographic, bottom). "
	"Binary files use morph.fs.readFile(path) and morph.fs.writeFile(path, "
	"ArrayBufferOrUint8Array); text files use morph.fs.readText and "
	"morph.fs.writeText. Example image tool: async function run(args) { "
	"await morph.image.resize({input: args.input, output: args.output, "
	"width: 512}); return { output: args.output }; }. Example canvas tool: "
	"async function run(args) { const c = morph.canvas.create({width: 256, "
	"height: 128}); const ctx = c.getContext(\"2d\"); ctx.fillStyle = "
	"\"#ffffff\"; ctx.fillRect(0, 0, 256, 128); ctx.fillStyle = "
	"\"#111111\"; ctx.fillText(args.text, 20, 70); "
	"morph.canvas.toFile({canvas: c, output: args.output}); return { "
	"output: args.output }; }. "
	"Example wasm: const mod = await WebAssembly.instantiate(bytes, {}); "
	"const n = mod.instance.exports.add(1, 2). Available host APIs: "
	"morph.fs.readText(path), morph.fs.writeText(path, text), "
	"morph.env.get(name), morph.exec(command), morph.fetch(url). Host API "
	"capabilities are controlled by the active dynamic_tools profile, not "
	"by tool_create arguments. If a dynamic tool with the same name already "
	"exists, tool_create updates it; non-dynamic tools cannot be "
	"overwritten. Every successful create/update records a checkpoint; use "
	"tool_history, tool_diff, and tool_rollback to inspect or rewind dynamic "
	"tool changes.";

static int tool_create_exec(const char *args, struct tool_result *result, void *user_data)
{
	return dynamic_tool_service_execute(user_data, DYNAMIC_TOOL_CREATE, args, result);
}

static int tool_promote_exec(const char *args, struct tool_result *result, void *user_data)
{
	return dynamic_tool_service_execute(user_data, DYNAMIC_TOOL_PROMOTE, args, result);
}

static int tool_delete_exec(const char *args, struct tool_result *result, void *user_data)
{
	return dynamic_tool_service_execute(user_data, DYNAMIC_TOOL_DELETE, args, result);
}

static int tool_history_exec(const char *args, struct tool_result *result, void *user_data)
{
	return dynamic_tool_service_execute(user_data, DYNAMIC_TOOL_HISTORY, args, result);
}

static int tool_diff_exec(const char *args, struct tool_result *result, void *user_data)
{
	return dynamic_tool_service_execute(user_data, DYNAMIC_TOOL_DIFF, args, result);
}

static int tool_rollback_exec(const char *args, struct tool_result *result, void *user_data)
{
	return dynamic_tool_service_execute(user_data, DYNAMIC_TOOL_ROLLBACK, args, result);
}

static void dynamic_tools_context_destroy(void *user_data)
{
	struct dynamic_tools_context *ctx = user_data;
	if (ctx && __atomic_sub_fetch(&ctx->refs, 1, __ATOMIC_ACQ_REL) == 0)
		free(ctx);
}

static void dynamic_tools_context_cleanup(struct dynamic_tools_context **owner)
{
	dynamic_tools_context_destroy(*owner);
	*owner = NULL;
}

static int dynamic_tools_register(struct tool_registry *reg, const struct tool_spec *descriptor,
				  struct dynamic_tools_context *ctx)
{
	if (!descriptor || descriptor->user_data_destroy || !ctx)
		return -EINVAL;
	struct tool_spec owner MORPH_CLEANUP(tool_spec_cleanup) = *descriptor;
	__atomic_add_fetch(&ctx->refs, 1, __ATOMIC_RELAXED);
	owner.user_data = ctx;
	owner.user_data_destroy = dynamic_tools_context_destroy;
	return tool_register_take(reg, &owner);
}

int dynamic_tools_set_session_id(struct tool_registry *reg, const char *session_id)
{
	struct tool_entry *entry;
	struct dynamic_tools_context *ctx;
	int rc;

	if (!reg)
		return -EINVAL;
	entry = tool_borrow(reg, "tool_create");
	if (!entry)
		return 0;
	ctx = entry->user_data;
	if (!ctx)
		return -EINVAL;
	snprintf(ctx->session_id, sizeof(ctx->session_id), "%s", session_id ? session_id : "");
	if (!ctx->session_id[0])
		return 0;
	rc = dynamic_tool_service_load_session(reg, ctx->tctx, ctx->cfg, ctx->session_id);
	return rc < 0 ? rc : 0;
}

int dynamic_tools_load_persistent(struct tool_registry *reg, struct tool_context *tctx,
				  const struct config_dynamic_tools *cfg)
{
	if (!cfg)
		return 0;
	return dynamic_tool_service_load_root(reg, tctx, cfg, cfg->persistent_dir,
					      TOOL_ORIGIN_DYNAMIC_PERSISTENT);
}

int dynamic_tools_init(struct tool_registry *reg, struct tool_context *tctx,
		       const struct config_dynamic_tools *cfg, const char *session_id)
{
	struct dynamic_tools_context *ctx MORPH_CLEANUP(dynamic_tools_context_cleanup) = NULL;
	int rc;

	if (!reg || !cfg)
		return -EINVAL;
	if (!cfg->enabled)
		return 0;
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;
	ctx->refs = 1;
	ctx->reg = reg;
	ctx->tctx = tctx;
	ctx->cfg = cfg;
	snprintf(ctx->session_id, sizeof(ctx->session_id), "%s", session_id ? session_id : "");
	struct tool_spec spec = {
		.origin = TOOL_ORIGIN_BUILTIN,
		.name = "tool_create",
		.description = TOOL_CREATE_DESCRIPTION,
		.input_schema = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":"
				"\"string\",\"description\":\"New tool name: lowercase letters, "
				"digits, underscore "
				"only.\"},\"description\":{\"type\":\"string\",\"description\":"
				"\"Short model-facing description of what the new tool "
				"does.\"},\"input_schema\":{\"type\":[\"object\",\"string\"],"
				"\"description\":\"JSON Schema for the new tool's "
				"arguments.\"},\"output_schema\":{\"type\":[\"object\",\"string\"],"
				"\"description\":\"JSON Schema for the new tool's result "
				"data.\"},\"source_js\":{\"type\":\"string\",\"description\":"
				"\"JavaScript code defining global "
				"run(args).\"}},\"required\":[\"name\",\"input_schema\",\"output_"
				"schema\",\"source_js\"],\"additionalProperties\":false}",
		.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
		.exec = tool_create_exec,
		.user_data = ctx,
		.user_data_destroy = NULL,
	};
	rc = dynamic_tools_register(reg, &spec, ctx);
	if (rc < 0)
		return rc;
	rc = dynamic_tools_register(
		reg,
		&(struct tool_spec){.origin = TOOL_ORIGIN_BUILTIN,
				    .name = "tool_promote",
				    .description = "Promote a session dynamic tool into the "
						   "persistent dynamic tool directory.",
				    .input_schema =
					    "{\"type\":\"object\",\"properties\":{\"name\":{"
					    "\"type\":\"string\"}},\"required\":[\"name\"]}",
				    .output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
				    .exec = tool_promote_exec,
				    .user_data = ctx,
				    .user_data_destroy = NULL},
		ctx);
	if (rc < 0)
		return rc;
	rc = dynamic_tools_register(
		reg,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.name = "tool_delete",
			.description = "Delete a dynamic tool. Session tools are removed from the "
				       "current session directory; persistent dynamic tools are "
				       "removed from the persistent dynamic tool directory. "
				       "Built-in, MCP, and ext tools cannot be deleted.",
			.input_schema = "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":"
					"\"string\"}},\"required\":[\"name\"]}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = tool_delete_exec,
			.user_data = ctx,
			.user_data_destroy = NULL},
		ctx);
	if (rc < 0)
		return rc;
	rc = dynamic_tools_register(
		reg,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.name = "tool_history",
			.description = "List checkpoints recorded for a session dynamic tool.",
			.input_schema = "{\"type\":\"object\",\"properties\":{\"name\":{"
					"\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}"
					"},\"required\":[\"name\"]}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = tool_history_exec,
			.user_data = ctx,
			.user_data_destroy = NULL},
		ctx);
	if (rc < 0)
		return rc;
	rc = dynamic_tools_register(
		reg,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.name = "tool_diff",
			.description = "Show a unified diff between a dynamic tool checkpoint "
				       "and the current tool files.",
			.input_schema = "{\"type\":\"object\",\"properties\":{\"name\":{"
					"\"type\":\"string\"},\"checkpoint_id\":{\"type\":"
					"\"string\",\"description\":\"Checkpoint id or "
					"latest.\"}},\"required\":[\"name\"]}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = tool_diff_exec,
			.user_data = ctx,
			.user_data_destroy = NULL},
		ctx);
	if (rc < 0)
		return rc;
	rc = dynamic_tools_register(
		reg,
		&(struct tool_spec){
			.origin = TOOL_ORIGIN_BUILTIN,
			.name = "tool_rollback",
			.description = "Roll back a session dynamic tool to a checkpoint. "
				       "Rolling back a creation removes the tool.",
			.input_schema = "{\"type\":\"object\",\"properties\":{\"name\":{"
					"\"type\":\"string\"},\"checkpoint_id\":{\"type\":"
					"\"string\",\"description\":\"Checkpoint id or "
					"latest.\"}},\"required\":[\"name\"]}",
			.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA,
			.exec = tool_rollback_exec,
			.user_data = ctx,
			.user_data_destroy = NULL},
		ctx);
	if (rc < 0)
		return rc;
	rc = dynamic_tools_load_persistent(reg, tctx, cfg);
	if (rc < 0)
		return rc;
	return dynamic_tool_service_load_session(reg, tctx, cfg, ctx->session_id);
}
