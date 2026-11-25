#include "persistence/orm/fcgi_canvas.h"
#include "sapi/fastcgi/session_store_internal.h"
#include "persistence/orm/fcgi_models.h"
#include "persistence/orm/records.h"
#include "cJSON.h"
#include <nb/random.h>
#include <stdio.h>
#include <stdlib.h>

int canvas_list_json(struct session_store *store, const char *session_id, char **out_json)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_rows *rows NB_CLEANUP(orm_rows_cleanup) = NULL;
	struct norm_error error = {0};
	cJSON *root NB_CLEANUP(cJSON_DeleteOwner) = NULL;

	if (!store || !session_id || !out_json)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(&store->db, &scope);
	const struct norm_order order = {
		.column = norm_col(&orm_fcgi_canvas_nodes, 11),
		.direction = NORM_ASC,
	};
	const struct norm_select_query query = {
		.where = norm_eq(norm_col(&orm_fcgi_canvas_nodes, 1), orm_string(session_id)),
		.order = &order,
		.order_count = 1,
		.max_rows = SIZE_MAX,
		.max_bytes = SIZE_MAX,
	};

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_select(scope.connection, &orm_fcgi_canvas_nodes, &query, NULL,
					    &rows, &error),
				&error);

	if (rc != 0)
		return rc;

	root = cJSON_CreateObject();
	cJSON *nodes = root ? cJSON_AddArrayToObject(root, "nodes") : NULL;

	if (!nodes)
		return NB_ERR_NOMEM;

	for (size_t i = 0; i < norm_rows_count(rows); i++) {
		const struct orm_fcgi_canvas_nodes_row *row = norm_rows_at(rows, i);
		cJSON *item = cJSON_CreateObject();

		if (!item)
			return NB_ERR_NOMEM;

		cJSON_AddStringToObject(item, "id", row->id.ptr);
		if (row->missing_parent_id)
			cJSON_AddNullToObject(item, "parent");
		else
			cJSON_AddStringToObject(item, "parent", row->parent_id.ptr);

		cJSON_AddStringToObject(item, "kind", row->kind.ptr);
		cJSON_AddNumberToObject(item, "x", row->x);
		cJSON_AddNumberToObject(item, "y", row->y);
		cJSON_AddNumberToObject(item, "w", row->w);
		cJSON_AddNumberToObject(item, "h", row->h);
		if (row->missing_content_ref)
			cJSON_AddNullToObject(item, "content_ref");
		else
			cJSON_AddStringToObject(item, "content_ref", row->content_ref.ptr);

		cJSON *metadata =
			!row->missing_meta && row->meta.len ? cJSON_Parse(row->meta.ptr) : NULL;

		if (metadata)
			cJSON_AddItemToObject(item, "meta", metadata);
		else
			cJSON_AddNullToObject(item, "meta");

		cJSON_AddNumberToObject(item, "updated_at", (double)row->updated_at);
		cJSON_AddItemToArray(nodes, item);
	}

	char *json = cJSON_PrintUnformatted(root);

	if (!json)
		return NB_ERR_NOMEM;

	*out_json = json;
	return 0;
}

static int mk_node_id(char out[40])
{
	nb_buf random NB_CLEANUP(nb_buf_destroy) = {0};
	int rc = nb_random_hex(&random, 12, NULL);

	if (rc != NB_OK)
		return rc;

	int written =
		snprintf(out, 40, "n_%llx_%s", (long long)fcgi_store_now(), nb_buf_cstr(&random));

	return written > 0 && written < 40 ? NB_OK : NB_ERR_RANGE;
}

int canvas_add_node(struct session_store *store, const char *session_id, const char *node_json,
		    char out_node_id[40])
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};

	if (!store || !session_id || !out_node_id)
		return NB_ERR_INVAL;

	int rc = mk_node_id(out_node_id);

	if (rc == 0)
		rc = orm_scope_begin(&store->db, &scope);

	if (rc != 0)
		return rc;

	const struct orm_fcgi_canvas_nodes_row row = {
		.id = nb_str_from_cstr(out_node_id),
		.session_id = nb_str_from_cstr(session_id),
		.missing_parent_id = true,
		.kind = NB_STR_LIT("node"),
		.missing_content_ref = true,
		.meta = nb_str_from_cstr(node_json ? node_json : "{}"),
		.created_at = fcgi_store_now(),
		.updated_at = fcgi_store_now(),
	};

	return orm_status(&store->db,
			  norm_insert(scope.connection, &orm_fcgi_canvas_nodes, &row, NULL, NULL,
				      NULL, &error),
			  &error);
}

int canvas_patch_node(struct session_store *store, const char *session_id, const char *node_id,
		      const char *patch_json)
{
	struct orm_scope scope NB_CLEANUP(orm_scope_cleanup) = {0};
	struct norm_error error = {0};
	struct norm_write_result result = {0};

	if (!store || !session_id || !node_id)
		return NB_ERR_INVAL;

	int rc = orm_scope_begin(&store->db, &scope);
	const struct norm_assignment assignments[] = {
		{.column = norm_col(&orm_fcgi_canvas_nodes, 9),
		 .value = orm_string(patch_json ? patch_json : "{}")},
		{.column = norm_col(&orm_fcgi_canvas_nodes, 11),
		 .value = norm_i64(fcgi_store_now())},
	};
	const struct norm_expr predicates[] = {
		norm_eq(norm_col(&orm_fcgi_canvas_nodes, 0), orm_string(node_id)),
		norm_eq(norm_col(&orm_fcgi_canvas_nodes, 1), orm_string(session_id)),
	};
	const struct norm_update_query query = {
		.set = assignments,
		.set_count = 2,
		.where = norm_and(predicates, 2),
	};

	if (rc == 0)
		rc = orm_status(&store->db,
				norm_update(scope.connection, &orm_fcgi_canvas_nodes, &query, NULL,
					    &result, &error),
				&error);

	return rc != 0 ? rc : result.affected_rows > 0 ? 0 : NB_ERR_NOT_FOUND;
}
