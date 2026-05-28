#ifndef IMG_ANNOTATE_H
#define IMG_ANNOTATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "agent/tool.h"

typedef void (*img_annotate_pause_fn)(void *user_data);
typedef void (*img_annotate_resume_fn)(void *user_data);

int img_annotate_init(struct tool_registry *reg,
		      img_annotate_pause_fn pause_fn,
		      img_annotate_resume_fn resume_fn,
		      void *user_data);

#ifdef __cplusplus
}
#endif

#endif
