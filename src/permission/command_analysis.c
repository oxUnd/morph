#include "command_analysis.h"
#include "errors.h"
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-bash.h>

#define BASH_PARSE_MAX_DEPTH 256

static int is_static_program_char(unsigned char ch)
{
	return (ch >= 'a' && ch <= 'z') ||
		(ch >= 'A' && ch <= 'Z') ||
		(ch >= '0' && ch <= '9') ||
		strchr("_./+@:-", (int)ch) != NULL;
}

static int quoted_program_is_static(TSNode node)
{
	const char *type = ts_node_type(node);
	uint32_t count;

	if (strcmp(type, "raw_string") == 0)
		return 1;
	if (strcmp(type, "string") != 0)
		return 0;
	count = ts_node_named_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		TSNode child = ts_node_named_child(node, i);

		if (strcmp(ts_node_type(child), "string_content") != 0)
			return 0;
	}
	return 1;
}

static int copy_static_program(const char *source, TSNode node,
			       char *out, size_t out_size)
{
	TSNode literal;
	uint32_t start;
	uint32_t end;
	size_t len;
	size_t offset = 0;
	int quoted = 0;

	if (!source || ts_node_is_null(node) || !out || out_size == 0)
		return NB_ERR_INVAL;
	if (ts_node_named_child_count(node) != 1)
		return NB_ERR_INVAL;
	literal = ts_node_named_child(node, 0);
	start = ts_node_start_byte(node);
	end = ts_node_end_byte(node);
	if (end <= start)
		return NB_ERR_NOT_FOUND;
	len = (size_t)(end - start);
	if (len >= 2 &&
	    ((source[start] == '\'' && source[end - 1] == '\'') ||
	     (source[start] == '"' && source[end - 1] == '"'))) {
		if (!quoted_program_is_static(literal))
			return NB_ERR_INVAL;
		quoted = 1;
		offset = 1;
		len -= 2;
	}
	if (len == 0 || len + 1 > out_size)
		return len == 0 ? NB_ERR_NOT_FOUND : NB_ERR_NAME_TOO_LONG;
	for (size_t i = 0; i < len; i++) {
		unsigned char ch =
			(unsigned char)source[(size_t)start + offset + i];

		if ((!quoted && !is_static_program_char(ch)) ||
		    (quoted && source[start] == '"' && ch == '\\'))
			return NB_ERR_INVAL;
	}
	memcpy(out, source + (size_t)start + offset, len);
	out[len] = '\0';
	return 0;
}

static int node_marks_compound(const char *type)
{
	static const char *const types[] = {
		"pipeline",
		"list",
		"subshell",
		"command_substitution",
		"process_substitution",
		"if_statement",
		"for_statement",
		"c_style_for_statement",
		"while_statement",
		"case_statement",
		"function_definition",
		"coproc",
		"negated_command",
		NULL
	};

	for (const char *const *item = types; *item; item++) {
		if (strcmp(type, *item) == 0)
			return 1;
	}
	return 0;
}

static int append_command(const char *source, TSNode node,
			  struct bash_parse_result *result)
{
	struct bash_parse_command *command;
	TSNode name_node;
	const char *base;
	size_t len;

	command = nb_vec_push(&result->commands);
	if (!command)
		return NB_ERR_NOMEM;
	memset(command, 0, sizeof(*command));
	command->start_byte = (size_t)ts_node_start_byte(node);
	command->end_byte = (size_t)ts_node_end_byte(node);
	name_node = ts_node_child_by_field_name(node, "name", 4);
	if (copy_static_program(source, name_node, command->program,
				sizeof(command->program)) != 0)
		return 0;
	base = strrchr(command->program, '/');
	base = base ? base + 1 : command->program;
	len = strlen(base);
	if (len == 0 || len >= sizeof(command->name)) {
		command->program[0] = '\0';
		return 0;
	}
	memcpy(command->name, base, len + 1);
	return 0;
}

static int visit_node(const char *source, TSNode node,
		      struct bash_parse_result *result, unsigned int depth)
{
	const char *type;
	uint32_t count;

	if (depth > BASH_PARSE_MAX_DEPTH) {
		result->has_error = 1;
		return NB_ERR_RANGE;
	}
	type = ts_node_type(node);
	if (node_marks_compound(type))
		result->is_compound = 1;
	if (strcmp(type, "command") == 0) {
		int rc = append_command(source, node, result);

		if (rc != 0)
			return rc;
	}
	count = ts_node_named_child_count(node);
	for (uint32_t i = 0; i < count; i++) {
		int rc = visit_node(source, ts_node_named_child(node, i),
				    result, depth + 1);

		if (rc != 0)
			return rc;
	}
	return 0;
}

int bash_parse_analyze(const char *source, struct bash_parse_result *result)
{
	TSParser *parser;
	TSTree *tree;
	TSNode root;
	size_t source_len;
	int rc;

	if (!source || !result)
		return NB_ERR_INVAL;
	memset(result, 0, sizeof(*result));
	rc = (nb_vec_init(&result->commands, sizeof(struct bash_parse_command)),
	      nb_vec_reserve(&result->commands, 4) ? 0 : NB_ERR_NOMEM);
	if (rc != 0)
		return rc;
	source_len = strlen(source);
	if (source_len > UINT32_MAX) {
		bash_parse_result_cleanup(result);
		return NB_ERR_TOO_BIG;
	}
	parser = ts_parser_new();
	if (!parser) {
		bash_parse_result_cleanup(result);
		return NB_ERR_NOMEM;
	}
	if (!ts_parser_set_language(parser, tree_sitter_bash())) {
		ts_parser_delete(parser);
		bash_parse_result_cleanup(result);
		return NB_ERR_INVAL;
	}
	tree = ts_parser_parse_string(parser, NULL, source,
				      (uint32_t)source_len);
	if (!tree) {
		ts_parser_delete(parser);
		bash_parse_result_cleanup(result);
		return NB_ERR_NOMEM;
	}
	root = ts_tree_root_node(tree);
	result->has_error = ts_node_has_error(root) ? 1 : 0;
	rc = visit_node(source, root, result, 0);
	if (rc == 0 && result->commands.len > 1)
		result->is_compound = 1;
	ts_tree_delete(tree);
	ts_parser_delete(parser);
	if (rc != 0)
		bash_parse_result_cleanup(result);
	return rc;
}

void bash_parse_result_cleanup(struct bash_parse_result *result)
{
	if (!result)
		return;
	nb_vec_cleanup(&result->commands);
	result->is_compound = 0;
	result->has_error = 0;
}

const struct bash_parse_command *
bash_parse_first_command_borrow(const struct bash_parse_result *result)
{
	if (!result || result->commands.len == 0)
		return NULL;
	return nb_vec_at(&result->commands, 0);
}

static int copy_first_field(const char *source, char *out, size_t out_size,
			    int use_name)
{
	struct bash_parse_result result;
	const struct bash_parse_command *command;
	const char *value;
	size_t len;
	int rc;

	if (!source || !out || out_size == 0)
		return NB_ERR_INVAL;
	rc = bash_parse_analyze(source, &result);
	if (rc != 0)
		return rc;
	command = bash_parse_first_command_borrow(&result);
	if (!command || result.has_error) {
		rc = NB_ERR_NOT_FOUND;
	} else {
		value = use_name ? command->name : command->program;
		len = strlen(value);
		if (len == 0)
			rc = NB_ERR_INVAL;
		else if (len + 1 > out_size)
			rc = NB_ERR_NAME_TOO_LONG;
		else {
			memcpy(out, value, len + 1);
			rc = 0;
		}
	}
	bash_parse_result_cleanup(&result);
	return rc;
}

int bash_parse_command_name(const char *source, char *out, size_t out_size)
{
	return copy_first_field(source, out, out_size, 1);
}

int bash_parse_command_program(const char *source, char *out, size_t out_size)
{
	return copy_first_field(source, out, out_size, 0);
}
