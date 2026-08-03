#ifndef VIDEO_RENDER_H
#define VIDEO_RENDER_H

#ifdef __cplusplus
extern "C" {
#endif

int video_play(const char *path, const char *mpv_args);
int video_render_terminal_preview(const char *path);

#ifdef __cplusplus
}
#endif

#endif
