#include "image.h"

#include <string.h>

enum image_format image_format_detect(const unsigned char *data, size_t len)
{
	if (!data)
		return MORPH_IMAGE_UNKNOWN;

	if (len >= 8 && data[0] == 0x89 && !memcmp(data + 1, "PNG", 3))
		return MORPH_IMAGE_PNG;

	if (len >= 2 && data[0] == 0xff && data[1] == 0xd8)
		return MORPH_IMAGE_JPEG;

	if (len >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4))
		return MORPH_IMAGE_WEBP;

	if (len >= 6 && (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6)))
		return MORPH_IMAGE_GIF;

	if (len >= 2 && data[0] == 'B' && data[1] == 'M')
		return MORPH_IMAGE_BMP;

	return MORPH_IMAGE_UNKNOWN;
}

enum image_format image_format_from_mime(nb_str mime)
{
	size_t separator = nb_str_find_char(mime, ';');
	if (separator != SIZE_MAX)
		mime.len = separator;

	mime = nb_str_trim(mime);
	const char *types[] = {
		"image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp"
	};
	const enum image_format formats[] = {
		MORPH_IMAGE_PNG, MORPH_IMAGE_JPEG, MORPH_IMAGE_WEBP,
		MORPH_IMAGE_GIF, MORPH_IMAGE_BMP
	};

	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		if (nb_str_eq_icase(mime, nb_str_from_cstr(types[i])))
			return formats[i];
	}

	return MORPH_IMAGE_UNKNOWN;
}

const char *image_format_extension(enum image_format format)
{
	switch (format) {
	case MORPH_IMAGE_PNG:
		return "png";
	case MORPH_IMAGE_JPEG:
		return "jpg";
	case MORPH_IMAGE_WEBP:
		return "webp";
	case MORPH_IMAGE_GIF:
		return "gif";
	case MORPH_IMAGE_BMP:
		return "bmp";
	case MORPH_IMAGE_UNKNOWN:
	default:
		return NULL;
	}
}
