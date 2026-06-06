#ifndef VIDEO_GEN_H
#define VIDEO_GEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>

struct model;

struct video_result {
	char path[PATH_MAX];
	char url[PATH_MAX];
	char error_msg[1024];
	int duration_seconds;
	int status;
};

int video_gen_create(struct model *self, const char *prompt,
		    const char **image_paths, int num_images,
		    const char **video_paths, int num_videos,
		    int duration, const char *output_dir,
		    struct video_result *result);

#ifdef __cplusplus
}
#endif

#endif
