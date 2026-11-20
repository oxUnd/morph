#ifndef MORPH_CLI_SERVICES_FILE_PREVIEW_H
#define MORPH_CLI_SERVICES_FILE_PREVIEW_H

#include <nb/buf.h>

struct cli_file_preview {
	char *path;
	nb_buf content;
	char *highlighted;
	int status, image, markdown, truncated;
};

struct cli_file_preview_job;

/* Async local file reads; workers never access views or the Application. */
int cli_file_preview_start(const char *path, struct cli_file_preview_job **out);
/* NB_ERR_AGAIN while running; success consumes the job and transfers the result. */
int cli_file_preview_collect(struct cli_file_preview_job **job, struct cli_file_preview **out);
void cli_file_preview_stop(struct cli_file_preview_job **job);
void cli_file_preview_free(struct cli_file_preview *preview);
void cli_file_preview_cleanup(struct cli_file_preview **preview);

#endif
