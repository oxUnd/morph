#include "video_gen.h"
#include "video_provider.h"
#include "models/llm.h"
#include "util/error.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define VIDEO_VOLCENGINE_ADAPTER "volcengine-videos"

int video_gen_adapter_supported(const char *provider, const char *adapter)
{
	const char *name;

	if (!provider)
		return 0;
	name = adapter && adapter[0] ? adapter : provider;
	if (strcmp(name, VIDEO_VOLCENGINE_ADAPTER) == 0)
		return 1;
	return (!adapter || !adapter[0]) && strcmp(name, "volcengine") == 0;
}

static const struct video_provider_ops *video_provider_resolve(const struct model *model)
{
	if (!model || !video_gen_adapter_supported(model->provider, model->adapter))
		return NULL;
	return video_volcengine_provider_borrow();
}

const char *video_gen_adapter_name(const struct model *model)
{
	const struct video_provider_ops *ops = video_provider_resolve(model);

	return ops ? ops->name : NULL;
}

int video_gen_capabilities(const struct model *model, struct video_capabilities *caps)
{
	const struct video_provider_ops *ops;

	if (!model || !caps)
		return -EINVAL;
	ops = video_provider_resolve(model);
	if (!ops)
		return -ENOTSUP;
	return ops->capabilities(model, caps);
}

static int video_capability_error(struct model *self, struct video_result *result,
				  const char *capability)
{
	const char *adapter = video_gen_adapter_name(self);
	const char *model_id = self && self->model_id[0] ? self->model_id : "(not configured)";

	snprintf(result->error_msg, sizeof(result->error_msg),
		 "video adapter '%s' does not support %s for model '%s'",
		 adapter ? adapter : "(unknown)", capability, model_id);
	if (self)
		snprintf(self->last_error, sizeof(self->last_error), "%s", result->error_msg);
	return -ENOTSUP;
}

int video_gen_create(struct model *self, const char *prompt, const char **image_paths,
		     int num_images, const char **video_paths, int num_videos,
		     const char **audio_paths, int num_audios, int generate_audio, int duration,
		     const char *output_dir, struct video_result *result)
{
	const struct video_provider_ops *ops;
	struct video_capabilities caps;
	int rc;

	if (!self || !prompt || !result || num_images < 0 || num_videos < 0 || num_audios < 0 ||
	    num_images > VIDEO_GEN_MAX_REFERENCE_IMAGES ||
	    num_videos > VIDEO_GEN_MAX_REFERENCE_VIDEOS ||
	    num_audios > VIDEO_GEN_MAX_REFERENCE_AUDIOS || (num_images > 0 && !image_paths) ||
	    (num_videos > 0 && !video_paths) || (num_audios > 0 && !audio_paths) ||
	    generate_audio < -1 || generate_audio > 1)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	if (!self->model_id[0]) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "video_gen: no model configured");
		return MORPH_ERR_NOT_CONFIGURED;
	}
	for (int i = 0; i < num_images; i++)
		if (!image_paths[i] || !image_paths[i][0])
			return -EINVAL;
	for (int i = 0; i < num_videos; i++)
		if (!video_paths[i] || !video_paths[i][0])
			return -EINVAL;
	for (int i = 0; i < num_audios; i++)
		if (!audio_paths[i] || !audio_paths[i][0])
			return -EINVAL;

	ops = video_provider_resolve(self);
	if (!ops) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "unsupported video adapter for provider '%s'", self->provider);
		return -ENOTSUP;
	}
	rc = ops->capabilities(self, &caps);
	if (rc != 0) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "unsupported video adapter for provider '%s'", self->provider);
		return rc;
	}
	if (num_images > 0 && !caps.supports_reference_images)
		return video_capability_error(self, result, "reference images");
	if (num_images > 1 && !caps.supports_multi_reference_images)
		return video_capability_error(self, result, "multiple reference images");
	if (num_videos > 0 && !caps.supports_reference_videos)
		return video_capability_error(self, result, "reference videos");
	if (num_audios > 0 && !caps.supports_reference_audios)
		return video_capability_error(self, result, "reference audios");
	if (num_audios > 0 && num_images == 0 && num_videos == 0) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "reference audio requires a reference image or video");
		return -EINVAL;
	}
	if (num_audios > 0 && generate_audio == 0) {
		snprintf(result->error_msg, sizeof(result->error_msg),
			 "reference audio requires generate_audio=true");
		return -EINVAL;
	}
	if (generate_audio == 1 && !caps.supports_generate_audio)
		return video_capability_error(self, result, "audio generation");
	if (num_audios > 0)
		generate_audio = 1;
	else if (generate_audio < 0 && caps.supports_generate_audio)
		generate_audio = 1;

	return ops->execute(self, prompt, image_paths, num_images, video_paths, num_videos,
			    audio_paths, num_audios, generate_audio, &caps, duration, output_dir,
			    result);
}
