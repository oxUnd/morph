#include "annotation.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static int annotation_number(const struct json *object, const char *name, double *out)
{
	const struct json *value = json_member(object, name);

	if (!json_is_number_value(value) || !isfinite(json_number_value(value)) ||
	    json_number_value(value) < 0 || json_number_value(value) > INT_MAX)
		return -EINVAL;
	*out = json_number_value(value);
	return 0;
}

static int annotation_index(const struct json *object, int count, int *out)
{
	double number;
	int rc = annotation_number(object, "image_index", &number);

	if (rc != 0 || number >= count || number != floor(number))
		return -EINVAL;
	*out = (int)number;
	return 0;
}

static int annotation_images(const struct json *items, struct annotation *out)
{
	const struct json *item;

	if (!json_is_array_value(items) || json_count(items) == 0)
		return -EINVAL;
	json_foreach (item, items) {
		struct annotation_image image = {0};
		const struct json *width = json_member(item, "width");
		const struct json *height = json_member(item, "height");
		struct annotation_image *slot;

		image.path = json_string_value(json_member(item, "path"));
		if (!image.path || !*image.path)
			return -EINVAL;
		if (width) {
			if (!json_is_number_value(width) || json_number_value(width) < 0 ||
			    json_number_value(width) > INT_MAX ||
			    json_number_value(width) != floor(json_number_value(width)))
				return -EINVAL;
			image.width = json_int_value(width);
		}
		if (height) {
			if (!json_is_number_value(height) || json_number_value(height) < 0 ||
			    json_number_value(height) > INT_MAX ||
			    json_number_value(height) != floor(json_number_value(height)))
				return -EINVAL;
			image.height = json_int_value(height);
		}
		slot = morph_array_push(&out->images);
		if (!slot)
			return -ENOMEM;
		*slot = image;
	}
	return 0;
}

static int annotation_bboxes(const struct json *items, struct annotation *out)
{
	const struct json *item;

	if (!items)
		return 0;
	if (!json_is_array_value(items))
		return -EINVAL;
	json_foreach (item, items) {
		struct annotation_bbox bbox = {0};
		struct annotation_bbox *slot;

		if (annotation_index(item, (int)out->images.nelts, &bbox.image_index) != 0 ||
		    annotation_number(item, "x", &bbox.rect.x) != 0 ||
		    annotation_number(item, "y", &bbox.rect.y) != 0 ||
		    annotation_number(item, "w", &bbox.rect.width) != 0 ||
		    annotation_number(item, "h", &bbox.rect.height) != 0 || bbox.rect.width <= 0 ||
		    bbox.rect.height <= 0)
			return -EINVAL;
		bbox.label = json_str_or(item, "label", "");
		slot = morph_array_push(&out->bboxes);
		if (!slot)
			return -ENOMEM;
		*slot = bbox;
	}
	return 0;
}

static int annotation_point_parse(const struct json *item, int count,
				  struct annotation_point *point)
{
	if (annotation_index(item, count, &point->image_index) != 0 ||
	    annotation_number(item, "x", &point->x) != 0 ||
	    annotation_number(item, "y", &point->y) != 0)
		return -EINVAL;
	return 0;
}

static int annotation_arrows(const struct json *items, struct annotation *out)
{
	const struct json *item;

	if (!items)
		return 0;
	if (!json_is_array_value(items))
		return -EINVAL;
	json_foreach (item, items) {
		struct annotation_arrow arrow = {0};
		struct annotation_arrow *slot;
		int count = (int)out->images.nelts;

		if (annotation_point_parse(json_member(item, "from"), count, &arrow.from) != 0 ||
		    annotation_point_parse(json_member(item, "to"), count, &arrow.to) != 0)
			return -EINVAL;
		arrow.label = json_str_or(item, "label", "");
		slot = morph_array_push(&out->arrows);
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
	json_free(annotation->owned);
	memset(annotation, 0, sizeof(*annotation));
}

int annotation_parse(const struct json *input, struct annotation *out)
{
	const struct json *body;
	struct annotation_image image;
	struct annotation_bbox bbox;
	struct annotation_arrow arrow;
	int rc;

	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (!json_is_object_value(input))
		return -EINVAL;
	body = json_member(input, "annotation");
	if (!body)
		body = input;
	if (json_is_string_value(body)) {
		out->owned = json_parse_prefix(json_string_value(body), NULL, NULL);
		body = out->owned;
	}
	if (!json_is_object_value(body)) {
		annotation_cleanup(out);
		return -EINVAL;
	}
	rc = morph_array_init(&out->images, 4, sizeof(image));
	if (rc == 0)
		rc = morph_array_init(&out->bboxes, 4, sizeof(bbox));
	if (rc == 0)
		rc = morph_array_init(&out->arrows, 4, sizeof(arrow));
	if (rc == 0)
		rc = annotation_images(json_member(body, "images"), out);
	if (rc == 0)
		rc = annotation_bboxes(json_member(body, "bboxes"), out);
	if (rc == 0)
		rc = annotation_arrows(json_member(body, "arrows"), out);
	if (rc != 0)
		annotation_cleanup(out);
	return rc;
}

const struct annotation_image *annotation_image_at(const struct annotation *annotation, int index)
{
	if (!annotation || index < 0)
		return NULL;
	return morph_array_get(&annotation->images, (size_t)index);
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
			other = morph_array_get(&annotation->arrows, j);
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
