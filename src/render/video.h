#ifndef VIDEO_RENDER_H
#define VIDEO_RENDER_H

struct morph_log;
#ifdef __cplusplus
extern "C" {
#endif

int video_play(struct morph_log *logger, const char *path, const char *mpv_args);
int video_render_terminal_preview(struct morph_log *logger, const char *path);

#ifdef __cplusplus
}
#endif

#endif
