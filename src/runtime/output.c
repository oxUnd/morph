#include "runtime/output.h"

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

static int output_id_for_path(struct db *db, const char *path)
{
	sqlite3_stmt *stmt = NULL;
	int id = 0;

	if (!db || !db->handle || !path)
		return 0;
	if (sqlite3_prepare_v2(db->handle,
		"SELECT id FROM outputs WHERE path=? ORDER BY id DESC LIMIT 1",
		-1, &stmt, NULL) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		id = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return id;
}

static void add_reference(struct db *db, cJSON *references, const char *kind,
			  const char *path)
{
	cJSON *item;
	cJSON *existing;
	int id;

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
		cJSON_AddNumberToObject(item, "output_id", id);
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
	const char *kind;
	const char *prompt = json_string(arguments, "prompt");
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (artifact->kind == TOOL_ARTIFACT_IMAGE) {
		kind = "image";
		model = context->config ? &context->config->models.image : NULL;
	} else if (artifact->kind == TOOL_ARTIFACT_VIDEO) {
		kind = "video";
		model = context->config ? &context->config->models.video : NULL;
	} else {
		return 0;
	}
	if (!prompt || !prompt[0])
		prompt = context->request_prompt;
	rc = sqlite3_prepare_v2(context->db->handle,
		"INSERT INTO outputs(session_id,kind,path,prompt,model,request_prompt,"
		"provider,tool_name,tool_call_id,turn_id,recipe_json,mime,width,height,"
		"duration_seconds,size_bytes,created_at) "
		"SELECT ?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,? "
		"WHERE NOT EXISTS(SELECT 1 FROM outputs WHERE path=?)",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		return -EIO;
#define BIND_TEXT(index, value) sqlite3_bind_text(stmt, index, (value) ? (value) : "", -1, SQLITE_TRANSIENT)
	if (context->session_id > 0)
		sqlite3_bind_int64(stmt, 1, context->session_id);
	else
		sqlite3_bind_null(stmt, 1);
	BIND_TEXT(2, kind);
	BIND_TEXT(3, artifact->path);
	BIND_TEXT(4, prompt);
	BIND_TEXT(5, model ? model->model : NULL);
	BIND_TEXT(6, context->request_prompt);
	BIND_TEXT(7, model ? model->provider : NULL);
	BIND_TEXT(8, event->tool_name);
	BIND_TEXT(9, event->tool_call_id);
	BIND_TEXT(10, context->turn_id);
	BIND_TEXT(11, recipe_json);
	BIND_TEXT(12, artifact->mime);
	sqlite3_bind_int(stmt, 13, artifact->width);
	sqlite3_bind_int(stmt, 14, artifact->height);
	sqlite3_bind_int(stmt, 15, artifact->duration_seconds);
	sqlite3_bind_int64(stmt, 16, artifact->size_bytes);
	sqlite3_bind_int64(stmt, 17, (sqlite3_int64)time(NULL));
	BIND_TEXT(18, artifact->path);
#undef BIND_TEXT
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_DONE && sqlite3_changes(context->db->handle) > 0)
		*output_id = sqlite3_last_insert_rowid(context->db->handle);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : -EIO;
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

static void add_column(cJSON *root, sqlite3_stmt *stmt, int index,
		       const char *name)
{
	const unsigned char *value = sqlite3_column_text(stmt, index);
	if (value && value[0])
		cJSON_AddStringToObject(root, name, (const char *)value);
}

char *runtime_output_get_json_by_path(struct db *db, const char *path)
{
	sqlite3_stmt *stmt = NULL;
	cJSON *root;
	char *json = NULL;
	const unsigned char *recipe;

	if (!db || !db->handle || !path || !path[0])
		return NULL;
	if (sqlite3_prepare_v2(db->handle,
		"SELECT id,session_id,kind,path,prompt,model,request_prompt,provider,"
		"tool_name,tool_call_id,turn_id,recipe_json,mime,width,height,"
		"duration_seconds,size_bytes,created_at FROM outputs "
		"WHERE path=? ORDER BY id DESC LIMIT 1", -1, &stmt, NULL) != SQLITE_OK)
		return NULL;
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
	if (sqlite3_step(stmt) != SQLITE_ROW)
		goto out;
	root = cJSON_CreateObject();
	if (!root)
		goto out;
	cJSON_AddNumberToObject(root, "id",
				(double)sqlite3_column_int64(stmt, 0));
	cJSON_AddNumberToObject(root, "session_id",
				(double)sqlite3_column_int64(stmt, 1));
	add_column(root, stmt, 2, "kind");
	add_column(root, stmt, 3, "path");
	add_column(root, stmt, 4, "prompt");
	add_column(root, stmt, 5, "model");
	add_column(root, stmt, 6, "request_prompt");
	add_column(root, stmt, 7, "provider");
	add_column(root, stmt, 8, "tool_name");
	add_column(root, stmt, 9, "tool_call_id");
	add_column(root, stmt, 10, "turn_id");
	recipe = sqlite3_column_text(stmt, 11);
	if (recipe) {
		cJSON *parsed = cJSON_Parse((const char *)recipe);
		if (parsed)
			cJSON_AddItemToObject(root, "recipe", parsed);
	}
	add_column(root, stmt, 12, "mime");
	cJSON_AddNumberToObject(root, "width", sqlite3_column_int(stmt, 13));
	cJSON_AddNumberToObject(root, "height", sqlite3_column_int(stmt, 14));
	cJSON_AddNumberToObject(root, "duration_seconds",
				sqlite3_column_int(stmt, 15));
	cJSON_AddNumberToObject(root, "size_bytes",
				(double)sqlite3_column_int64(stmt, 16));
	cJSON_AddNumberToObject(root, "created_at",
				(double)sqlite3_column_int64(stmt, 17));
	json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
out:
	sqlite3_finalize(stmt);
	return json;
}

char *runtime_output_get_json_from_database(const char *database_path,
					    const char *artifact_path)
{
	struct db db;
	char *json = NULL;

	if (!database_path || !artifact_path)
		return NULL;
	if (db_open(&db, database_path) != 0)
		return NULL;
	if (db_init_schema(&db) == 0)
		json = runtime_output_get_json_by_path(&db, artifact_path);
	db_close(&db);
	return json;
}
