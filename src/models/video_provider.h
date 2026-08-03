#ifndef VIDEO_PROVIDER_H
#define VIDEO_PROVIDER_H

#ifdef __cplusplus
extern "C" {
#endif

struct model;
struct video_result;

struct video_capabilities {
	int supports_generate;
	int supports_reference_images;
	int supports_multi_reference_images;
	int supports_generate_audio;
	int supports_reference_videos;
	int supports_reference_audios;
};

struct video_provider_ops {
	const char *name;
	int (*capabilities)(const struct model *model,
			    struct video_capabilities *caps);
	int (*execute)(struct model *self, const char *prompt,
		       const char **image_paths, int num_images,
		       const char **video_paths, int num_videos,
		       const char **audio_paths, int num_audios,
		       int generate_audio,
		       const struct video_capabilities *caps,
		       int duration, const char *output_dir,
		       struct video_result *result);
};

#ifdef __cplusplus
}
#endif

#endif
