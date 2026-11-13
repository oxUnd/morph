#ifndef SAMPLE_CUSTOMIZATION_APPLICATION_H
#define SAMPLE_CUSTOMIZATION_APPLICATION_H

#include <morph/tui.h>

#define SAMPLE_CUSTOMIZATION_READY 1
#define SAMPLE_CUSTOMIZATION_REDRAW 2

struct sample_customization_application {
	struct morph_tui_application base;
	nb_buf name;
	int value;
	int step;
	unsigned name_revision;
};

const struct morph_tui_application_ops *sample_customization_application_methods(void);
int sample_customization_application_set_name(struct sample_customization_application *,
					      const char *name);

#endif
