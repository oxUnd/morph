#include "annotation.h"
#include "util/cleanup.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static int annotation_number(const cJSON *object, const char *name, double *out)
{
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);

	if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < 0 ||
	    value->valuedouble > INT_MAX)
		return -EINVAL;
	*out = value->valuedouble;
	return 0;
}

static int annotation_index(const cJSON *object, int count, int *out)
{
	double number;
	int rc = annotation_number(object, "image_index", &number);

	if (rc != 0 || number >= count || number != floor(number))
		return -EINVAL;
	*out = (int)number;
	return 0;
}

static const char *annotation_label(const cJSON *object)
{
	const char *label = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, "label"));

	return label ? label : "";
}

static int annotation_images(const cJSON *items, struct annotation *out)
{
	const cJSON *item;

	if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0)
		return -EINVAL;
	cJSON_ArrayForEach (item, items) {
		struct annotation_image image = {0};
		const cJSON *width = cJSON_GetObjectItemCaseSensitive(item, "width");
		const cJSON *height = cJSON_GetObjectItemCaseSensitive(item, "height");
		struct annotation_image *slot;

		image.path = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "path"));
		if (!image.path || !*image.path)
			return -EINVAL;
		if (width) {
			if (!cJSON_IsNumber(width) || width->valuedouble < 0 ||
			    width->valuedouble > INT_MAX ||
			    width->valuedouble != floor(width->valuedouble))
				return -EINVAL;
			image.width = width->valueint;
		}
		if (height) {
			if (!cJSON_IsNumber(height) || height->valuedouble < 0 ||
			    height->valuedouble > INT_MAX ||
			    height->valuedouble != floor(height->valuedouble))
				return -EINVAL;
			image.height = height->valueint;
		}
		slot = morph_array_push_borrow(&out->images);
		if (!slot)
			return -ENOMEM;
		*slot = image;
	}
	return 0;
}

static int annotation_bboxes(const cJSON *items, struct annotation *out)
{
	const cJSON *item;

	if (!items)
		return 0;
	if (!cJSON_IsArray(items))
		return -EINVAL;
	cJSON_ArrayForEach (item, items) {
		struct annotation_bbox bbox = {0};
		struct annotation_bbox *slot;

		if (annotation_index(item, (int)out->images.nelts, &bbox.image_index) != 0 ||
		    annotation_number(item, "x", &bbox.rect.x) != 0 ||
		    annotation_number(item, "y", &bbox.rect.y) != 0 ||
		    annotation_number(item, "w", &bbox.rect.width) != 0 ||
		    annotation_number(item, "h", &bbox.rect.height) != 0 || bbox.rect.width <= 0 ||
		    bbox.rect.height <= 0)
			return -EINVAL;
		bbox.label = annotation_label(item);
		slot = morph_array_push_borrow(&out->bboxes);
		if (!slot)
			return -ENOMEM;
		*slot = bbox;
	}
	return 0;
}

static int annotation_point_parse(const cJSON *item, int count, struct annotation_point *point)
{
	if (annotation_index(item, count, &point->image_index) != 0 ||
	    annotation_number(item, "x", &point->x) != 0 ||
	    annotation_number(item, "y", &point->y) != 0)
		return -EINVAL;
	return 0;
}

static int annotation_arrows(const cJSON *items, struct annotation *out)
{
	const cJSON *item;

	if (!items)
		return 0;
	if (!cJSON_IsArray(items))
		return -EINVAL;
	cJSON_ArrayForEach (item, items) {
		struct annotation_arrow arrow = {0};
		struct annotation_arrow *slot;
		int count = (int)out->images.nelts;

		if (annotation_point_parse(cJSON_GetObjectItemCaseSensitive(item, "from"), count,
					   &arrow.from) != 0 ||
		    annotation_point_parse(cJSON_GetObjectItemCaseSensitive(item, "to"), count,
					   &arrow.to) != 0)
			return -EINVAL;
		arrow.label = annotation_label(item);
		slot = morph_array_push_borrow(&out->arrows);
		if (!slot)
			return -ENOMEM;
		*slot = arrow;
	}
	return 0;
}

void annotation_cleanup(struct annotation *annotation)
{
	if (!annotation)
		return;
	morph_array_cleanup(&annotation->arrows);
	morph_array_cleanup(&annotation->bboxes);
	morph_array_cleanup(&annotation->images);
	cJSON_Delete(annotation->owned);
	memset(annotation, 0, sizeof(*annotation));
}

int annotation_parse_copy(const cJSON *input, struct annotation *out)
{
	struct annotation candidate MORPH_CLEANUP(annotation_cleanup) = {0};
	const cJSON *body;
	struct annotation_image image;
	struct annotation_bbox bbox;
	struct annotation_arrow arrow;
	int rc;

	if (!out || !cJSON_IsObject(input))
		return -EINVAL;
	if (out->owned || out->images.elts || out->bboxes.elts || out->arrows.elts)
		return -EALREADY;
	body = cJSON_GetObjectItemCaseSensitive(input, "annotation");
	if (!body)
		body = input;
	if (cJSON_IsString(body)) {
		candidate.owned = cJSON_ParseWithOpts(body->valuestring, NULL, 1);
		if (!candidate.owned)
			return -EINVAL;
	} else {
		candidate.owned = cJSON_Duplicate(body, 1);
		if (!candidate.owned)
			return -ENOMEM;
	}
	body = candidate.owned;
	if (!cJSON_IsObject(body))
		return -EINVAL;
	rc = morph_array_init(&candidate.images, 4, sizeof(image));
	if (rc == 0)
		rc = morph_array_init(&candidate.bboxes, 4, sizeof(bbox));
	if (rc == 0)
		rc = morph_array_init(&candidate.arrows, 4, sizeof(arrow));
	if (rc == 0)
		rc = annotation_images(cJSON_GetObjectItemCaseSensitive(body, "images"),
				       &candidate);
	if (rc == 0)
		rc = annotation_bboxes(cJSON_GetObjectItemCaseSensitive(body, "bboxes"),
				       &candidate);
	if (rc == 0)
		rc = annotation_arrows(cJSON_GetObjectItemCaseSensitive(body, "arrows"),
				       &candidate);
	if (rc != 0)
		return rc;
	*out = candidate;
	memset(&candidate, 0, sizeof(candidate));
	return 0;
}

const struct annotation_image *annotation_image_borrow(const struct annotation *annotation,
						       int index)
{
	if (!annotation || index < 0)
		return NULL;
	return morph_array_borrow(&annotation->images, (size_t)index);
}

int annotation_rect_normalize(const struct annotation_rect *input, int width, int height,
			      struct annotation_rect *out)
{
	if (!input || !out || !isfinite(input->x) || !isfinite(input->y) ||
	    !isfinite(input->width) || !isfinite(input->height) || input->x < 0 || input->y < 0 ||
	    input->width <= 0 || input->height <= 0)
		return -EINVAL;
	*out = *input;
	if (input->x <= 1 && input->y <= 1 && input->width <= 1 && input->height <= 1)
		return 0;
	if (width <= 0 || height <= 0)
		return -EINVAL;
	out->x /= width;
	out->y /= height;
	out->width /= width;
	out->height /= height;
	return 0;
}

int annotation_point_pixels(const struct annotation_point *point, int width, int height, double *x,
			    double *y)
{
	if (!point || !x || !y || width <= 0 || height <= 0 || !isfinite(point->x) ||
	    !isfinite(point->y) || point->x < 0 || point->y < 0)
		return -EINVAL;
	*x = point->x;
	*y = point->y;
	if (*x <= 1 && *y <= 1) {
		*x *= width;
		*y *= height;
	}
	return 0;
}

int annotation_main_image(const struct annotation *annotation)
{
	const struct annotation_arrow *arrow;
	int main_index = -1;
	int best = 0;

	morph_array_foreach (arrow, &annotation->arrows, const struct annotation_arrow) {
		const struct annotation_arrow *other;
		int count = 0;

		for (size_t j = 0; j < annotation->arrows.nelts; j++) {
			other = morph_array_borrow(&annotation->arrows, j);
			if (other->to.image_index == arrow->to.image_index)
				count++;
		}
		if (count > best ||
		    (count == best && (main_index < 0 || arrow->to.image_index < main_index))) {
			best = count;
			main_index = arrow->to.image_index;
		}
	}
	return main_index;
}
