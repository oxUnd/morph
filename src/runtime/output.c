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
	return name && (!strcmp(name, "img_gen") ||
			!strcmp(name, "img_inpaint") ||
			!strcmp(name, "img_compose") ||
			!strcmp(name, "vid_gen"));
}

static const char *json_string(cJSON *object, const char *name)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
	return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int64_t output_id_for_path(struct db *db, const char *path)
{
	int64_t id = 0;

	(void)output_store_find_id(db, path, &id);
	return id;
}

static void add_reference(struct db *db, cJSON *references, const char *kind,
			  const char *path)
{
	cJSON *item;
	cJSON *existing;
	int64_t id;

	if (!references || !path || !path[0])
		return;
	cJSON_ArrayForEach(existing, references) {
		const char *old = json_string(existing, "path");
		if (old && !strcmp(old, path))
			return;
	}
	item = cJSON_CreateObject();
	if (!item)
		return;
	cJSON_AddStringToObject(item, "kind", kind);
	cJSON_AddStringToObject(item, "path", path);
	id = output_id_for_path(db, path);
	if (id > 0)
		cJSON_AddNumberToObject(item, "output_id", (double)id);
	cJSON_AddItemToArray(references, item);
}

static void add_reference_value(struct db *db, cJSON *references,
				cJSON *value, const char *kind)
{
	cJSON *item;

	if (cJSON_IsString(value)) {
		add_reference(db, references, kind, value->valuestring);
		return;
	}
	if (!cJSON_IsArray(value))
		return;
	cJSON_ArrayForEach(item, value) {
		if (cJSON_IsString(item))
			add_reference(db, references, kind, item->valuestring);
	}
}

static void add_annotation_references(struct db *db, cJSON *references,
				      cJSON *arguments)
{
	cJSON *annotation = cJSON_GetObjectItemCaseSensitive(arguments,
						     "annotation");
	cJSON *owned = NULL;
	cJSON *images;
	cJSON *image;

	if (cJSON_IsString(annotation)) {
		owned = cJSON_Parse(annotation->valuestring);
		annotation = owned;
	} else if (!cJSON_IsObject(annotation)) {
		annotation = arguments;
	}
	images = cJSON_IsObject(annotation)
		? cJSON_GetObjectItemCaseSensitive(annotation, "images") : NULL;
	if (cJSON_IsArray(images)) {
		cJSON_ArrayForEach(image, images) {
			const char *path = json_string(image, "path");
			add_reference(db, references, "image", path);
		}
	}
	cJSON_Delete(owned);
}

static char *build_recipe(struct db *db, const char *tool_name,
			  cJSON *arguments)
{
	cJSON *recipe = cJSON_CreateObject();
	cJSON *references = cJSON_CreateArray();
	char *json;

	if (!recipe || !references) {
		cJSON_Delete(recipe);
		cJSON_Delete(references);
		return NULL;
	}
	cJSON_AddStringToObject(recipe, "schema", "morph.generation_recipe.v1");
	cJSON_AddStringToObject(recipe, "tool", tool_name);
	cJSON_AddItemToObject(recipe, "arguments", cJSON_Duplicate(arguments, 1));
	add_reference_value(db, references,
		cJSON_GetObjectItemCaseSensitive(arguments, "reference_image"),
		"image");
	add_reference_value(db, references,
		cJSON_GetObjectItemCaseSensitive(arguments, "reference_images"),
		"image");
	add_reference_value(db, references,
		cJSON_GetObjectItemCaseSensitive(arguments, "reference_videos"),
		"video");
	add_reference_value(db, references,
		cJSON_GetObjectItemCaseSensitive(arguments, "reference_audios"),
		"audio");
	if (!strcmp(tool_name, "img_inpaint") ||
	    !strcmp(tool_name, "img_compose"))
		add_annotation_references(db, references, arguments);
	cJSON_AddItemToObject(recipe, "references", references);
	json = cJSON_PrintUnformatted(recipe);
	cJSON_Delete(recipe);
	return json;
}

static int insert_output(const struct runtime_output_context *context,
			 const struct react_output_event *event,
			 const struct tool_artifact *artifact, cJSON *arguments,
			 const char *recipe_json, int64_t *output_id)
{
	const struct config_model_entry *model;
	const char *prompt = json_string(arguments, "prompt");
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

static void emit_output_created(const struct runtime_output_context *context,
				int64_t id,
				const struct tool_artifact *artifact)
{
	cJSON *data = cJSON_CreateObject();
	struct morph_event ev;

	if (!data)
		return;
	cJSON_AddNumberToObject(data, "output_id", (double)id);
	cJSON_AddNumberToObject(data, "session_id", (double)context->session_id);
	cJSON_AddStringToObject(data, "kind",
		artifact->kind == TOOL_ARTIFACT_VIDEO ? "video" : "image");
	cJSON_AddStringToObject(data, "path", artifact->path);
	cJSON_AddNumberToObject(data, "width", artifact->width);
	cJSON_AddNumberToObject(data, "height", artifact->height);
	cJSON_AddNumberToObject(data, "duration_seconds",
				artifact->duration_seconds);
	memset(&ev, 0, sizeof(ev));
	ev.type = MORPH_EVENT_ARTIFACT;
	ev.name = "output.created";
	ev.phase = "ready";
	ev.message = "generation output recorded";
	ev.data = data;
	ev.turn_id = context->turn_id;
	(void)morph_event_emit(context->event_cb, context->event_user_data, &ev);
	cJSON_Delete(data);
}

int runtime_output_record_event(const struct runtime_output_context *context,
				const struct react_output_event *event)
{
	cJSON *arguments;
	char *recipe_json;
	int rc = 0;

	if (!context || !context->db || !context->db->handle || !event ||
	    event->type != REACT_STEP_OBSERVATION ||
	    event->status != REACT_OUTPUT_COMPLETED ||
	    !is_creative_tool(event->tool_name) || !event->tool_args ||
	    !event->artifacts)
		return 0;
	arguments = cJSON_Parse(event->tool_args);
	if (!arguments)
		return -EINVAL;
	recipe_json = build_recipe(context->db, event->tool_name, arguments);
	if (!recipe_json) {
		cJSON_Delete(arguments);
		return -ENOMEM;
	}
	for (int i = 0; i < event->artifacts->count; i++) {
		int64_t id = 0;
		int item_rc = insert_output(context, event,
			&event->artifacts->items[i], arguments, recipe_json, &id);
		if (item_rc != 0)
			rc = item_rc;
		else if (id > 0)
			emit_output_created(context, id,
				&event->artifacts->items[i]);
	}
	free(recipe_json);
	cJSON_Delete(arguments);
	return rc;
}

static int output_add_text(cJSON *root, const char *name, const char *value)
{
	return !value || !value[0] || cJSON_AddStringToObject(root, name, value);
}

char *runtime_output_get_json_by_path(struct db *db, const char *path)
{
	struct output_record record;
	struct arena *arena;
	cJSON *root;
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
	root = cJSON_CreateObject();
	ok = root && cJSON_AddNumberToObject(root, "id", (double)record.id) &&
	     cJSON_AddNumberToObject(root, "session_id", (double)record.session_id) &&
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
	     cJSON_AddNumberToObject(root, "width", record.width) &&
	     cJSON_AddNumberToObject(root, "height", record.height) &&
	     cJSON_AddNumberToObject(root, "duration_seconds", record.duration_seconds) &&
	     cJSON_AddNumberToObject(root, "size_bytes", (double)record.size_bytes) &&
	     cJSON_AddNumberToObject(root, "created_at", (double)record.created_at);
	if (ok && record.recipe_json) {
		cJSON *recipe = cJSON_Parse(record.recipe_json);

		if (recipe && !cJSON_AddItemToObject(root, "recipe", recipe)) {
			cJSON_Delete(recipe);
			ok = 0;
		}
	}
	if (ok)
		json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
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
