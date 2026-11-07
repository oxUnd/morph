#ifndef MORPH_TOOL_ANNOTATION_H
#define MORPH_TOOL_ANNOTATION_H

#include "cJSON.h"
#include "util/array.h"

#ifdef __cplusplus
extern "C" {
#endif

struct annotation_image {
	const char *path;
	int width;
	int height;
};

struct annotation_rect {
	double x;
	double y;
	double width;
	double height;
};

struct annotation_bbox {
	int image_index;
	struct annotation_rect rect;
	const char *label;
};

struct annotation_point {
	int image_index;
	double x;
	double y;
};

struct annotation_arrow {
	struct annotation_point from;
	struct annotation_point to;
	const char *label;
};

/* Owns an independent parsed tree; field views remain valid until cleanup. */
struct annotation {
	cJSON *owned;
	morph_array_t images;
	morph_array_t bboxes;
	morph_array_t arrows;
};

int annotation_parse_copy(const cJSON *input, struct annotation *out);
void annotation_cleanup(struct annotation *annotation);
const struct annotation_image *annotation_image_borrow(const struct annotation *annotation,
						       int index);
int annotation_rect_normalize(const struct annotation_rect *input, int width, int height,
			      struct annotation_rect *out);
int annotation_point_pixels(const struct annotation_point *point, int width, int height, double *x,
			    double *y);
int annotation_main_image(const struct annotation *annotation);

#ifdef __cplusplus
}
#endif

#endif
