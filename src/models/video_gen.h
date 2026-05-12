#ifndef VIDEO_GEN_H
#define VIDEO_GEN_H

#ifdef __cplusplus
extern "C" {
#endif

struct model;

struct video_result {
	char path[512];
	char url[512];
	int duration_seconds;
	int status;
};

int video_gen_create(struct model *self, const char *prompt,
		    const char *image_path, int duration,
		    struct video_result *result);

#ifdef __cplusplus
}
#endif

#endif