#include "template.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/str.h"
#include "util/utf8.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct template_file_id {
	dev_t device;
	ino_t inode;
};

struct template_context {
	int root_fd;
	const struct morph_prompt_template_variable *variables;
	size_t count;
	struct morph_prompt_template_error *error;
	morph_buf_t output;
	size_t source_bytes;
	size_t files;
	size_t depth;
	struct template_file_id active[MORPH_PROMPT_TEMPLATE_INCLUDE_DEPTH];
};

struct template_source {
	const char *path;
	const char *text;
	size_t position;
};

struct template_section {
	morph_str_t name;
	size_t opening;
	size_t depth;
	int conditional;
	int enabled;
};

static int template_fail(struct template_context *ctx,
		const struct template_source *source, size_t offset,
		int code, const char *message)
{
	if (ctx->error) {
		struct morph_prompt_template_error *error = ctx->error;
		snprintf(error->path, sizeof(error->path), "%s", source->path);
		error->line = 1;
		error->column = 1;
		for (size_t i = 0; source->text && i < offset; i++) {
			if (source->text[i] == '\n') {
				error->line++;
				error->column = 1;
			} else {
				error->column++;
			}
		}
		snprintf(error->message, sizeof(error->message), "%s", message);
	}
	return code;
}

static int identifier_start(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int identifier_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int variable_name_valid(morph_str_t name)
{
	int start = 1;
	for (size_t i = 0; i < name.len; i++) {
		char c = name.data[i];
		if (start && !identifier_start(c))
			return 0;
		if (!start && c != '.' && !identifier_start(c) && !identifier_digit(c))
			return 0;
		start = c == '.';
	}
	return name.len && !start;
}

static int template_path_valid(const char *path)
{
	if (!path || !path[0] || strlen(path) >= PATH_MAX)
		return 0;
	const char *component = path;
	for (const char *p = path;; p++) {
		char c = *p;
		if (c != '/' && c != '\0') {
			if (!identifier_start(c) && !identifier_digit(c) && c != '-' && c != '.')
				return 0;
			continue;
		}
		size_t len = (size_t)(p - component);
		if (!len || (len == 1 && component[0] == '.') ||
		    (len == 2 && component[0] == '.' && component[1] == '.'))
			return 0;
		if (!c)
			return 1;
		component = p + 1;
	}
}

/* Walk held directory descriptors; no component below root may be a symlink. */
static int template_open(struct template_context *ctx, const char *path)
{
	char *copy = strdup(path);
	if (!copy)
		return -ENOMEM;
	int directory = fcntl(ctx->root_fd, F_DUPFD_CLOEXEC, 0);
	if (directory < 0) {
		int rc = morph_error_from_errno(errno);
		free(copy);
		return rc;
	}
	char *component = copy;
	for (;;) {
		char *slash = strchr(component, '/');
		if (slash)
			*slash = '\0';
		int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
		if (slash)
			flags |= O_DIRECTORY;
		int next = openat(directory, component, flags);
		int rc = next < 0 ? morph_error_from_errno(errno) : 0;
		close(directory);
		if (rc != 0 || !slash) {
			free(copy);
			return rc != 0 ? rc : next;
		}
		directory = next;
		component = slash + 1;
	}
}

static int template_read(struct template_context *ctx, int fd, morph_buf_t *text)
{
	char chunk[BUFSIZ];
	for (;;) {
		ssize_t n = read(fd, chunk, sizeof(chunk));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return morph_error_from_errno(errno);
		}
		if (!n)
			return 0;
		size_t bytes = (size_t)n;
		if (bytes > MORPH_PROMPT_TEMPLATE_FILE_MAX - text->len ||
		    bytes > MORPH_PROMPT_TEMPLATE_TOTAL_MAX - ctx->source_bytes)
			return -EFBIG;
		ctx->source_bytes += bytes;
		int rc = morph_buf_append(text, chunk, bytes);
		if (rc != 0)
			return rc;
	}
}

static int append_text(struct template_context *ctx, struct template_source *source,
		const char *text, size_t len, int emit)
{
	if (!emit)
		return 0;
	if (len > MORPH_PROMPT_TEMPLATE_OUTPUT_MAX - ctx->output.len)
		return template_fail(ctx, source, source->position, -EFBIG,
			"Rendered prompt exceeds the output limit.");
	int rc = morph_buf_append(&ctx->output, text, len);
	if (rc != 0)
		return template_fail(ctx, source, source->position, rc,
			"Cannot allocate rendered prompt.");
	return 0;
}

static const struct morph_prompt_template_variable *find_variable(
		struct template_context *ctx, morph_str_t name)
{
	for (size_t i = 0; i < ctx->count; i++)
		if (strlen(ctx->variables[i].name) == name.len &&
		    memcmp(ctx->variables[i].name, name.data, name.len) == 0)
			return &ctx->variables[i];
	return NULL;
}

static int tag_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static morph_str_t trim_tag(morph_str_t tag)
{
	while (tag.len && tag_space(*tag.data)) {
		tag.data++;
		tag.len--;
	}
	while (tag.len && tag_space(tag.data[tag.len - 1]))
		tag.len--;
	return tag;
}

static int render_file(struct template_context *ctx, const char *path, int emit);

static int render_body(struct template_context *ctx, struct template_source *source,
		const struct template_section *section, int emit)
{
	const char *text = source->text;
	int parent_emit = emit;
	int alternate = 0;
	size_t depth = section ? section->depth : 0;
	if (section)
		emit = emit && section->enabled;
	for (;;) {
		size_t start = source->position;
		while (text[source->position] &&
		       strncmp(text + source->position, "{{", 2) != 0 &&
		       !(text[source->position] == '\\' &&
			 (text[source->position + 1] == '\\' ||
			  strncmp(text + source->position + 1, "{{", 2) == 0)))
			source->position++;
		int rc = append_text(ctx, source, text + start, source->position - start, emit);
		if (rc != 0)
			return rc;
		if (!text[source->position])
			return section ? template_fail(ctx, source, section->opening, MORPH_ERR_FORMAT,
				"Section is missing its closing tag.") : 0;
		if (text[source->position] == '\\') {
			source->position++;
			size_t len = text[source->position] == '\\' ? 1u : 2u;
			rc = append_text(ctx, source, text + source->position, len, emit);
			source->position += len;
			if (rc != 0)
				return rc;
			continue;
		}
		size_t offset = source->position;
		const char *end = strstr(text + offset + 2, "}}");
		if (!end)
			return template_fail(ctx, source, offset, MORPH_ERR_FORMAT,
				"Template tag is missing its closing delimiter.");
		morph_str_t tag = {.len = (size_t)(end - (text + offset + 2)),
				   .data = text + offset + 2};
		tag = trim_tag(tag);
		source->position = (size_t)(end - text) + 2;
		if (!tag.len)
			return template_fail(ctx, source, offset, MORPH_ERR_FORMAT, "Empty template tag.");
		if (section && section->conditional &&
		    morph_strcmp(tag, (morph_str_t)MORPH_STRLIT("else")) == 0) {
			if (alternate)
				return template_fail(ctx, source, offset, MORPH_ERR_FORMAT,
					"Conditional section has more than one else tag.");
			alternate = 1;
			emit = parent_emit && !section->enabled;
			continue;
		}
		char kind = *tag.data;
		if (kind == '!')
			continue;
		if (kind == '>' || kind == '#' || kind == '^' || kind == '/') {
			tag.data++;
			tag.len--;
			tag = trim_tag(tag);
		}
		if (kind == '>') {
			if (ctx->root_fd < 0)
				return template_fail(ctx, source, offset, MORPH_ERR_FORMAT,
					"Partials require a template bundle root.");
			if (tag.len >= PATH_MAX)
				return template_fail(ctx, source, offset, -ENAMETOOLONG,
					"Partial path exceeds the path limit.");
			char *path = strndup(tag.data, tag.len);
			if (!path)
				return template_fail(ctx, source, offset, -ENOMEM,
					"Cannot allocate partial path.");
			rc = template_path_valid(path) ? render_file(ctx, path, emit) :
				template_fail(ctx, source, offset, MORPH_ERR_FORMAT,
					"Partial path must stay below the template root.");
			free(path);
			if (rc != 0)
				return rc;
			continue;
		}
		morph_str_t closing = tag;
		int conditional = kind == '#' && tag.len >= 3 &&
			memcmp(tag.data, "if", 2) == 0 && tag_space(tag.data[2]);
		if (conditional) {
			tag.data += 2;
			tag.len -= 2;
			tag = trim_tag(tag);
			closing = (morph_str_t)MORPH_STRLIT("if");
		}
		if (!variable_name_valid(tag))
			return template_fail(ctx, source, offset, MORPH_ERR_FORMAT,
				"Invalid variable name or unsupported template syntax.");
		if (kind == '/') {
			if (!section || morph_strcmp(tag, section->name) != 0)
				return template_fail(ctx, source, offset, MORPH_ERR_FORMAT,
					"Closing tag does not match the current section.");
			return 0;
		}
		const struct morph_prompt_template_variable *variable = find_variable(ctx, tag);
		if (!variable)
			return template_fail(ctx, source, offset, MORPH_ERR_FORMAT,
				"Template references an undefined variable.");
		if (kind == '#' || kind == '^') {
			if (variable->type != MORPH_PROMPT_TEMPLATE_BOOL)
				return template_fail(ctx, source, offset, MORPH_ERR_FORMAT,
					"Sections require a boolean variable.");
			if (depth >= MORPH_PROMPT_TEMPLATE_SECTION_DEPTH)
				return template_fail(ctx, source, offset, -ELOOP,
					"Template section nesting limit exceeded.");
			int enabled = kind == '#' ? variable->boolean != 0 : variable->boolean == 0;
			struct template_section child = {.name = closing, .opening = offset,
				.depth = depth + 1, .conditional = conditional, .enabled = enabled};
			rc = render_body(ctx, source, &child, emit);
		} else {
			if (variable->type != MORPH_PROMPT_TEMPLATE_STRING)
				return template_fail(ctx, source, offset, MORPH_ERR_FORMAT,
					"Substitutions require a string variable.");
			rc = append_text(ctx, source, variable->string, strlen(variable->string), emit);
		}
		if (rc != 0)
			return rc;
	}
}

static int render_file(struct template_context *ctx, const char *path, int emit)
{
	struct template_source source = {.path = path};
	if (ctx->depth >= MORPH_PROMPT_TEMPLATE_INCLUDE_DEPTH ||
	    ctx->files >= MORPH_PROMPT_TEMPLATE_FILES_MAX)
		return template_fail(ctx, &source, 0, -ELOOP, "Template include limit exceeded.");
	int fd = template_open(ctx, path);
	if (fd < 0)
		return template_fail(ctx, &source, 0, fd, "Cannot open template file.");
	struct stat st;
	int rc = fstat(fd, &st) == 0 ? 0 : morph_error_from_errno(errno);
	if (rc == 0 && !S_ISREG(st.st_mode))
		rc = MORPH_ERR_FORMAT;
	if (rc == 0 && st.st_size > MORPH_PROMPT_TEMPLATE_FILE_MAX)
		rc = -EFBIG;
	for (size_t i = 0; rc == 0 && i < ctx->depth; i++)
		if (ctx->active[i].device == st.st_dev && ctx->active[i].inode == st.st_ino)
			rc = -ELOOP;
	if (rc != 0) {
		close(fd);
		return template_fail(ctx, &source, 0, rc,
			"Template must be a regular file within limits, without include cycles.");
	}
	morph_buf_t text = {0};
	rc = morph_buf_init(&text, 0);
	if (rc == 0)
		rc = template_read(ctx, fd, &text);
	if (close(fd) != 0 && rc == 0)
		rc = morph_error_from_errno(errno);
	if (rc == 0 && (memchr(text.data, '\0', text.len) || utf8nvalid(text.data, text.len)))
		rc = -EILSEQ;
	if (rc != 0) {
		morph_buf_cleanup(&text);
		return template_fail(ctx, &source, 0, rc, "Cannot read a valid UTF-8 template within limits.");
	}
	source.text = morph_buf_cstr(&text);
	ctx->active[ctx->depth].device = st.st_dev;
	ctx->active[ctx->depth].inode = st.st_ino;
	ctx->depth++;
	ctx->files++;
	rc = render_body(ctx, &source, NULL, emit);
	ctx->depth--;
	morph_buf_cleanup(&text);
	return rc;
}

static int validate_variables(struct template_context *ctx)
{
	if (ctx->count > MORPH_PROMPT_TEMPLATE_VARIABLES_MAX || (ctx->count && !ctx->variables))
		return -EINVAL;
	size_t bytes = 0;
	for (size_t i = 0; i < ctx->count; i++) {
		const struct morph_prompt_template_variable *variable = &ctx->variables[i];
		if (!variable->name || !variable_name_valid((morph_str_t){
			.len = strlen(variable->name), .data = variable->name}))
			return -EINVAL;
		for (size_t j = 0; j < i; j++)
			if (strcmp(variable->name, ctx->variables[j].name) == 0)
				return -EINVAL;
		if (variable->type == MORPH_PROMPT_TEMPLATE_BOOL)
			continue;
		if (variable->type != MORPH_PROMPT_TEMPLATE_STRING || !variable->string)
			return -EINVAL;
		size_t len = strlen(variable->string);
		if (len > MORPH_PROMPT_TEMPLATE_TOTAL_MAX - bytes)
			return -EFBIG;
		if (utf8valid(variable->string))
			return -EILSEQ;
		bytes += len;
	}
	return 0;
}

int morph_prompt_template_render_file(const char *root, const char *entry,
		const struct morph_prompt_template_variable *variables, size_t count,
		char **out, struct morph_prompt_template_error *error)
{
	if (error)
		memset(error, 0, sizeof(*error));
	if (!out)
		return -EINVAL;
	*out = NULL;
	struct template_context ctx = {.root_fd = -1, .variables = variables,
		.count = count, .error = error};
	struct template_source source = {.path = entry ? entry : ""};
	if (!root || !*root || !template_path_valid(entry))
		return template_fail(&ctx, &source, 0, -EINVAL, "Invalid template root or entry path.");
	int rc = validate_variables(&ctx);
	if (rc != 0)
		return template_fail(&ctx, &source, 0, rc, "Invalid template variable bindings.");
	ctx.root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (ctx.root_fd < 0) {
		rc = morph_error_from_errno(errno);
		return template_fail(&ctx, &source, 0, rc, "Cannot open template root.");
	}
	rc = morph_buf_init(&ctx.output, 0);
	if (rc == 0)
		rc = render_file(&ctx, entry, 1);
	else
		rc = template_fail(&ctx, &source, 0, rc, "Cannot allocate template output.");
	if (close(ctx.root_fd) != 0 && rc == 0)
		rc = template_fail(&ctx, &source, 0, morph_error_from_errno(errno),
			"Cannot close template root.");
	if (rc == 0)
		*out = morph_buf_detach(&ctx.output);
	morph_buf_cleanup(&ctx.output);
	return rc;
}

int morph_prompt_template_render(const char *text,
		const struct morph_prompt_template_variable *variables, size_t count,
		char **out, struct morph_prompt_template_error *error)
{
	if (error)
		memset(error, 0, sizeof(*error));
	if (!out)
		return -EINVAL;
	*out = NULL;
	struct template_context ctx = {.root_fd = -1, .variables = variables,
		.count = count, .error = error};
	struct template_source source = {.path = "<string>", .text = text};
	if (!text)
		return template_fail(&ctx, &source, 0, -EINVAL, "Template text is required.");
	size_t len = strnlen(text, MORPH_PROMPT_TEMPLATE_FILE_MAX + 1u);
	if (len > MORPH_PROMPT_TEMPLATE_FILE_MAX)
		return template_fail(&ctx, &source, 0, -EFBIG, "Template exceeds the source limit.");
	if (utf8nvalid(text, len))
		return template_fail(&ctx, &source, 0, -EILSEQ, "Template must contain valid UTF-8.");
	int rc = validate_variables(&ctx);
	if (rc != 0)
		return template_fail(&ctx, &source, 0, rc, "Invalid template variable bindings.");
	rc = morph_buf_init(&ctx.output, 0);
	if (rc == 0)
		rc = render_body(&ctx, &source, NULL, 1);
	else
		rc = template_fail(&ctx, &source, 0, rc, "Cannot allocate template output.");
	if (rc == 0)
		*out = morph_buf_detach(&ctx.output);
	morph_buf_cleanup(&ctx.output);
	return rc;
}
