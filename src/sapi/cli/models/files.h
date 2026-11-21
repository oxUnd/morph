#ifndef MORPH_CLI_MODELS_FILES_H
#define MORPH_CLI_MODELS_FILES_H

#include <nb/buf.h>
#include <nb/vec.h>
#include <stdint.h>

enum cli_files_kind { CLI_FILES_REFERENCED, CLI_FILES_GENERATED, CLI_FILES_MODIFIED };

struct cli_files_entry {
	char *path;
	enum cli_files_kind kind;
	int line;
};

struct cli_files_model {
	nb_vec entries;
	uint64_t revision;
	int truncated;
};

void cli_files_init(struct cli_files_model *);
void cli_files_cleanup(struct cli_files_model *);
void cli_files_move(struct cli_files_model *destination, struct cli_files_model *source);
/* Resolves a local link lexically, without reading the filesystem.
 * Web URLs and anchors return NB_ERR_UNSUPPORTED. Output must be empty. */
int cli_files_resolve(const char *directory, const char *url, nb_buf *path, int *line);
int cli_files_add(struct cli_files_model *, const char *directory, const char *url,
	enum cli_files_kind);
int cli_files_markdown(struct cli_files_model *, const char *directory, const char *text);
struct cli_transcript_tool;
int cli_files_tool(struct cli_files_model *, const char *directory,
	const struct cli_transcript_tool *);

#endif
