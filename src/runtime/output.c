#include "runtime/output.h"
#include "persistence/output_store.h"
#include "util/arena.h"

#include "util/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int is_creative_tool(const char *name)
{
	return name && (!strcmp(name, "img_gen") || !strcmp(name, "img_inpaint") ||
			!strcmp(name, "img_compose") || !strcmp(name, "vid_gen"));
}

static int64_t output_id_for_path(struct db *db, const char *path)
{
	int64_t id = 0;

	(void)output_store_find_id(db, path, &id);
	return id;
}

static void add_reference(struct db *db, struct json *references, const char *kind,
			  const char *path)
{
	struct json *item;
	struct json *existing;
	int64_t id;

	if (!references || !path || !path[0])
		return;
	json_foreach (existing, references) {
		const char *old = json_str(existing, "path");
		if (old && !strcmp(old, path))
			return;
	}
	item = json_object();
	if (!item)
		return;
	json_add_string(item, "kind", kind);
	json_add_string(item, "path", path);
	id = output_id_for_path(db, path);
	if (id > 0)
		json_add_number(item, "output_id", (double)id);
	if (json_array_add(references, item) != 0)
		json_free(item);
}

static void add_reference_value(struct db *db, struct json *references, struct json *value,
				const char *kind)
{
	struct json *item;

	if (json_is_string_value(value)) {
		add_reference(db, references, kind, json_string_value(value));
		return;
	}
	if (!json_is_array_value(value))
		return;
	json_foreach (item, value) {
		if (json_is_string_value(item))
			add_reference(db, references, kind, json_string_value(item));
	}
}

static void add_annotation_references(struct db *db, struct json *references,
				      struct json *arguments)
{
	struct json *annotation = json_member(arguments, "annotation");
	struct json *owned = NULL;
	struct json *images;
	struct json *image;

	if (json_is_string_value(annotation)) {
		owned = json_parse_prefix(json_string_value(annotation), NULL, NULL);
		annotation = owned;
	} else if (!json_is_object_value(annotation)) {
		annotation = arguments;
	}
	images = json_is_object_value(annotation) ? json_member(annotation, "images") : NULL;
	if (json_is_array_value(images)) {
		json_foreach (image, images) {
			const char *path = json_str(image, "path");
			add_reference(db, references, "image", path);
		}
	}
	json_free(owned);
}

static char *build_recipe(struct db *db, const char *tool_name, struct json *arguments)
{
	struct json *recipe = json_object();
	struct json *references = json_array();
	struct json *copied_arguments = json_clone(arguments, true);
	char *json = NULL;

	if (!recipe || !references || !copied_arguments)
		goto out_free_recipe;
	if (!json_add_string(recipe, "schema", "morph.generation_recipe.v1") ||
	    !json_add_string(recipe, "tool", tool_name) ||
	    json_object_add(recipe, "arguments", copied_arguments) != 0)
		goto out_free_recipe;
	add_reference_value(db, references, json_member(arguments, "reference_image"), "image");
	add_reference_value(db, references, json_member(arguments, "reference_images"), "image");
	add_reference_value(db, references, json_member(arguments, "reference_videos"), "video");
	add_reference_value(db, references, json_member(arguments, "reference_audios"), "audio");
	if (!strcmp(tool_name, "img_inpaint") || !strcmp(tool_name, "img_compose"))
		add_annotation_references(db, references, arguments);
	if (json_object_add(recipe, "references", references) != 0)
		goto out_free_recipe;
	json = json_stringify(recipe);
out_free_recipe:
	json_free(copied_arguments);
	json_free(references);
	json_free(recipe);
	return json;
}

static int insert_output(const struct runtime_output_context *context,
			 const struct react_output_event *event,
			 const struct tool_artifact *artifact, struct json *arguments,
			 const char *recipe_json, int64_t *output_id)
{
	const struct config_model_entry *model;
	const char *prompt = json_str(arguments, "prompt");
	struct output_record record = {0};

	if (artifact->kind == TOOL_ARTIFACT_IMAGE) {
		record.kind = "image";
		model = context->config ? &context->config->models.image : NULL;
	} else if (artifact->kind == TOOL_ARTIFACT_VIDEO) {
		record.kind = "video";
		model = context->config ? &context->config->models.video : NULL;
	} else {
		return 0;
	}
	record.session_id = context->session_id;
	record.path = artifact->path;
	record.prompt = prompt && prompt[0] ? prompt : context->request_prompt;
	record.model = model ? model->model : NULL;
	record.request_prompt = context->request_prompt;
	record.provider = model ? model->provider : NULL;
	record.tool_name = event->tool_name;
	record.tool_call_id = event->tool_call_id;
	record.turn_id = context->turn_id;
	record.recipe_json = recipe_json;
	record.mime = artifact->mime;
	record.width = artifact->width;
	record.height = artifact->height;
	record.duration_seconds = artifact->duration_seconds;
	record.size_bytes = artifact->size_bytes;
	record.created_at = (int64_t)time(NULL);
	return output_store_insert(context->db, &record, output_id);
}

static void emit_output_created(const struct runtime_output_context *context, int64_t id,
				const struct tool_artifact *artifact)
{
	struct json *data = json_object();
	struct morph_event ev;

	if (!data)
		return;
	json_add_number(data, "output_id", (double)id);
	json_add_number(data, "session_id", (double)context->session_id);
	json_add_string(data, "kind", artifact->kind == TOOL_ARTIFACT_VIDEO ? "video" : "image");
	json_add_string(data, "path", artifact->path);
	json_add_number(data, "width", artifact->width);
	json_add_number(data, "height", artifact->height);
	json_add_number(data, "duration_seconds", artifact->duration_seconds);
	memset(&ev, 0, sizeof(ev));
	ev.type = MORPH_EVENT_ARTIFACT;
	ev.name = "output.created";
	ev.phase = "ready";
	ev.message = "generation output recorded";
	ev.data = data;
	ev.turn_id = context->turn_id;
	(void)morph_event_emit(context->event_cb, context->event_user_data, &ev);
	json_free(data);
}

int runtime_output_record_event(const struct runtime_output_context *context,
				const struct react_output_event *event)
{
	struct json *arguments;
	char *recipe_json;
	int rc = 0;

	if (!context || !context->db || !context->db->handle || !event ||
	    event->type != REACT_STEP_OBSERVATION || event->status != REACT_OUTPUT_COMPLETED ||
	    !is_creative_tool(event->tool_name) || !event->tool_args || !event->artifacts)
		return 0;
	arguments = json_parse_prefix(event->tool_args, NULL, NULL);
	if (!arguments)
		return -EINVAL;
	recipe_json = build_recipe(context->db, event->tool_name, arguments);
	if (!recipe_json) {
		json_free(arguments);
		return -ENOMEM;
	}
	for (int i = 0; i < event->artifacts->count; i++) {
		int64_t id = 0;
		int item_rc = insert_output(context, event, &event->artifacts->items[i], arguments,
					    recipe_json, &id);
		if (item_rc != 0)
			rc = item_rc;
		else if (id > 0)
			emit_output_created(context, id, &event->artifacts->items[i]);
	}
	free(recipe_json);
	json_free(arguments);
	return rc;
}

static int output_add_text(struct json *root, const char *name, const char *value)
{
	return !value || !value[0] || json_add_string(root, name, value);
}

char *runtime_output_get_json_by_path(struct db *db, const char *path)
{
	struct output_record record;
	struct arena *arena;
	struct json *root;
	char *json = NULL;
	int ok;

	if (!db || !db->handle || !path || !path[0])
		return NULL;
	arena = arena_create(0);
	if (!arena)
		return NULL;
	if (output_store_get(db, path, arena, &record) != 0) {
		arena_destroy(arena);
		return NULL;
	}
	root = json_object();
	ok = root && json_add_number(root, "id", (double)record.id) &&
	     json_add_number(root, "session_id", (double)record.session_id) &&
	     output_add_text(root, "kind", record.kind) &&
	     output_add_text(root, "path", record.path) &&
	     output_add_text(root, "prompt", record.prompt) &&
	     output_add_text(root, "model", record.model) &&
	     output_add_text(root, "request_prompt", record.request_prompt) &&
	     output_add_text(root, "provider", record.provider) &&
	     output_add_text(root, "tool_name", record.tool_name) &&
	     output_add_text(root, "tool_call_id", record.tool_call_id) &&
	     output_add_text(root, "turn_id", record.turn_id) &&
	     output_add_text(root, "mime", record.mime) &&
	     json_add_number(root, "width", record.width) &&
	     json_add_number(root, "height", record.height) &&
	     json_add_number(root, "duration_seconds", record.duration_seconds) &&
	     json_add_number(root, "size_bytes", (double)record.size_bytes) &&
	     json_add_number(root, "created_at", (double)record.created_at);
	if (ok && record.recipe_json) {
		struct json *recipe = json_parse_prefix(record.recipe_json, NULL, NULL);

		if (recipe && json_object_add(root, "recipe", recipe) != 0) {
			json_free(recipe);
			ok = 0;
		}
	}
	if (ok)
		json = json_stringify(root);
	json_free(root);
	arena_destroy(arena);
	return json;
}

char *runtime_output_get_json_from_database(struct morph_log *logger, const char *database_path,
					    const char *artifact_path)
{
	struct db db;
	char *json = NULL;

	if (!database_path || !artifact_path)
		return NULL;
	if (db_open(logger, &db, database_path) != 0)
		return NULL;
	if (db_init_schema(&db) == 0)
		json = runtime_output_get_json_by_path(&db, artifact_path);
	db_close(&db);
	return json;
}
