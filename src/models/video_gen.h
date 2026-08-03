#ifndef VIDEO_GEN_H
#define VIDEO_GEN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>

struct model;
struct video_capabilities;

#define VIDEO_GEN_MAX_REFERENCE_IMAGES 16
#define VIDEO_GEN_MAX_REFERENCE_VIDEOS 8
#define VIDEO_GEN_MAX_REFERENCE_AUDIOS 3

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
		    const char **audio_paths, int num_audios,
		    /* -1 selects the adapter default; 0 disables; 1 enables. */
		    int generate_audio,
		    int duration, const char *output_dir,
		    struct video_result *result);

const char *video_gen_adapter_name(const struct model *model);
int video_gen_adapter_supported(const char *provider, const char *adapter);
int video_gen_capabilities(const struct model *model,
			   struct video_capabilities *caps);

#ifdef __cplusplus
}
#endif

#endif
