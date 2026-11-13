#ifndef VIDEO_RENDER_H
#define VIDEO_RENDER_H

struct nb_log;
#include "nb/buf.h"
#include "nb/cleanup.h"
#ifdef __cplusplus
extern "C" {
#endif

int video_play(struct nb_log *logger, const char *path, const char *mpv_args);
int video_render_terminal_preview(struct nb_log *logger, const char *path);
/* Creates one PNG preview without terminal output. Success returns a temporary
 * path owned by the caller, which must unlink it before freeing it. */
int video_preview_create_copy(const char *path, char **out_path);
int video_file_url_append(const char *path, nb_buf *url);

#ifdef __cplusplus
}
#endif

#endif
