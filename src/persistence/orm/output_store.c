#include "output_store.h"
#include "records.h"
#include "errors.h"
#include <norm/query.h>

struct output_row {
	int64_t id;
	int64_t session_id;
	bool session_missing;
	nb_str kind;
	bool kind_missing;
	nb_str path;
	bool path_missing;
	nb_str prompt;
	bool prompt_missing;
	nb_str model;
	bool model_missing;
	nb_str request_prompt;
	bool request_prompt_missing;
	nb_str provider;
	bool provider_missing;
	nb_str tool_name;
	bool tool_name_missing;
	nb_str tool_call_id;
	bool tool_call_id_missing;
	nb_str turn_id;
	bool turn_id_missing;
	nb_str recipe_json;
	bool recipe_json_missing;
	nb_str mime;
	bool mime_missing;
	int width;
	int height;
	int duration_seconds;
	int64_t size_bytes;
	int64_t created_at;
};

static const struct norm_field output_fields[] = {
	NORM_FIELD(struct output_row, id, NORM_TYPE_I64,
		   NORM_FIELD_PRIMARY_KEY | NORM_FIELD_GENERATED),
	NORM_NULLABLE_FIELD(struct output_row, session_id, session_missing, NORM_TYPE_I64, 0),
	NORM_NULLABLE_FIELD(struct output_row, kind, kind_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct output_row, path, path_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct output_row, prompt, prompt_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct output_row, model, model_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct output_row, request_prompt, request_prompt_missing,
			    NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct output_row, provider, provider_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct output_row, tool_name, tool_name_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct output_row, tool_call_id, tool_call_id_missing, NORM_TYPE_TEXT,
			    0),
	NORM_NULLABLE_FIELD(struct output_row, turn_id, turn_id_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct output_row, recipe_json, recipe_json_missing, NORM_TYPE_TEXT, 0),
	NORM_NULLABLE_FIELD(struct output_row, mime, mime_missing, NORM_TYPE_TEXT, 0),
	NORM_FIELD(struct output_row, width, NORM_TYPE_I32, 0),
	NORM_FIELD(struct output_row, height, NORM_TYPE_I32, 0),
	NORM_FIELD(struct output_row, duration_seconds, NORM_TYPE_I32, 0),
	NORM_FIELD(struct output_row, size_bytes, NORM_TYPE_I64, 0),
	NORM_FIELD(struct output_row, created_at, NORM_TYPE_I64, 0),
};

static const struct norm_model output_model =
	NORM_MODEL(struct output_row, "outputs", output_fields);

int output_store_find_id(struct db *db, const char *path, int64_t *id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;

	if (!path || !id)
		return NB_ERR_INVAL;

	*id = 0;
	const size_t field = 0;
	const struct norm_order order = {.column = norm_col(&output_model, 0),
					 .direction = NORM_DESC};
	const struct norm_select_query query = {
		.fields = &field,
		.field_count = 1,
		.where = norm_eq(norm_col(&output_model, 3), orm_string(path)),
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = 1,
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_select_rows(&scope, &output_model, &query, &rows);

	if (rc == 0 && norm_rows_count(rows))
		*id = ((const struct output_row *)norm_rows_at(rows, 0))->id;

	return rc;
}

int output_store_insert(struct db *db, const struct output_record_input *record, int64_t *id)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_write_result result = {0};

	if (!record || !record->path || !id)
		return NB_ERR_INVAL;

	*id = 0;
	const struct norm_value values[] = {
		record->session_id > 0 ? norm_i64(record->session_id) : norm_null(),
		orm_string(record->kind ? record->kind : ""),
		orm_string(record->path),
		orm_string(record->prompt ? record->prompt : ""),
		orm_string(record->model ? record->model : ""),
		orm_string(record->request_prompt ? record->request_prompt : ""),
		orm_string(record->provider ? record->provider : ""),
		orm_string(record->tool_name ? record->tool_name : ""),
		orm_string(record->tool_call_id ? record->tool_call_id : ""),
		orm_string(record->turn_id ? record->turn_id : ""),
		orm_string(record->recipe_json ? record->recipe_json : ""),
		orm_string(record->mime ? record->mime : ""),
		norm_i32(record->width),
		norm_i32(record->height),
		norm_i32(record->duration_seconds),
		norm_i64(record->size_bytes),
		norm_i64(record->created_at),
	};
	struct norm_query_field fields[NB_ARRAY_SIZE(values)] = {0};
	size_t target_fields[NB_ARRAY_SIZE(values)] = {0};
	const struct norm_model input_model = {
		.size = sizeof(struct output_row),
		.alignment = _Alignof(struct output_row),
		.fields = output_fields + 1,
		.field_count = NB_ARRAY_SIZE(values),
	};
	struct norm_table_ref existing = norm_table(&output_model, NB_STR_LIT("existing"));
	const struct norm_query_field one = {.value = norm_term_value(norm_i32(1)), .into = 0};
	const struct norm_query lookup = {
		.from = &existing,
		.projection = &orm_integer_model,
		.fields = &one,
		.field_count = 1,
		.where = norm_eq(norm_table_col(&existing, 3), orm_string(record->path)),
	};
	const struct norm_expr exists = norm_exists(&lookup);

	for (size_t i = 0; i < NB_ARRAY_SIZE(values); i++) {
		fields[i].value = norm_term_value(values[i]);
		fields[i].into = i;
		target_fields[i] = i + 1;
	}

	const struct norm_query insert = {
		.projection = &input_model,
		.fields = fields,
		.field_count = NB_ARRAY_SIZE(fields),
		.where = norm_not(&exists),
	};
	int rc = orm_scope_begin(db, &scope);

	if (rc == 0)
		rc = orm_insert_from(&scope, &output_model, target_fields,
				     NB_ARRAY_SIZE(target_fields), &insert, NULL, &result);

	if (rc == 0 && result.affected_rows && result.has_insert_id)
		*id = result.insert_id;

	return rc;
}

void output_record_cleanup(struct output_record *record)
{
	if (!record)
		return;

	free(record->kind);
	free(record->path);
	free(record->prompt);
	free(record->model);
	free(record->request_prompt);
	free(record->provider);
	free(record->tool_name);
	free(record->tool_call_id);
	free(record->turn_id);
	free(record->recipe_json);
	free(record->mime);
	memset(record, 0, sizeof(*record));
}

int output_store_copy(struct db *db, const char *path, struct output_record *out)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct output_record value NB_CLEANUP(output_record_cleanup) = {0};

	if (!path || !out)
		return NB_ERR_INVAL;

	if (out->kind || out->path || out->prompt || out->model || out->request_prompt ||
	    out->provider || out->tool_name || out->tool_call_id || out->turn_id ||
	    out->recipe_json || out->mime)
		return NB_ERR_EXISTS;

	int rc = orm_scope_begin(db, &scope);
	struct norm_error error = {0};
	struct norm_expr condition = norm_eq(norm_col(&output_model, 3), orm_string(path));
	struct norm_order order = {.column = norm_col(&output_model, 0), .direction = NORM_DESC};
	struct norm_select_query query = {
		.where = condition,
		.order = &order,
		.order_count = 1,
		.has_limit = true,
		.limit = 1,
	};

	if (rc == 0)
		rc = orm_status(db,
				norm_select(scope.connection, &output_model, &query,
					    orm_scope_call(&scope), &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	if (!norm_rows_count(rows))
		return NB_ERR_NO_DATA;

	const struct output_row *row = norm_rows_at(rows, 0);

	value.id = row->id;
	value.session_id = row->session_id;
	value.width = row->width;
	value.height = row->height;
	value.duration_seconds = row->duration_seconds;
	value.size_bytes = row->size_bytes;
	value.created_at = row->created_at;
	if (!row->kind_missing) {
		value.kind = orm_text_copy(row->kind);
		if (!value.kind)
			return NB_ERR_NOMEM;
	}

	if (!row->path_missing) {
		value.path = orm_text_copy(row->path);
		if (!value.path)
			return NB_ERR_NOMEM;
	}

	if (!row->prompt_missing) {
		value.prompt = orm_text_copy(row->prompt);
		if (!value.prompt)
			return NB_ERR_NOMEM;
	}

	if (!row->model_missing) {
		value.model = orm_text_copy(row->model);
		if (!value.model)
			return NB_ERR_NOMEM;
	}

	if (!row->request_prompt_missing) {
		value.request_prompt = orm_text_copy(row->request_prompt);
		if (!value.request_prompt)
			return NB_ERR_NOMEM;
	}

	if (!row->provider_missing) {
		value.provider = orm_text_copy(row->provider);
		if (!value.provider)
			return NB_ERR_NOMEM;
	}

	if (!row->tool_name_missing) {
		value.tool_name = orm_text_copy(row->tool_name);
		if (!value.tool_name)
			return NB_ERR_NOMEM;
	}

	if (!row->tool_call_id_missing) {
		value.tool_call_id = orm_text_copy(row->tool_call_id);
		if (!value.tool_call_id)
			return NB_ERR_NOMEM;
	}

	if (!row->turn_id_missing) {
		value.turn_id = orm_text_copy(row->turn_id);
		if (!value.turn_id)
			return NB_ERR_NOMEM;
	}

	if (!row->recipe_json_missing) {
		value.recipe_json = orm_text_copy(row->recipe_json);
		if (!value.recipe_json)
			return NB_ERR_NOMEM;
	}

	if (!row->mime_missing) {
		value.mime = orm_text_copy(row->mime);
		if (!value.mime)
			return NB_ERR_NOMEM;
	}

	*out = value;
	memset(&value, 0, sizeof(value));
	return 0;
}
