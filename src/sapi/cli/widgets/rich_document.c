#include "sapi/cli/widgets/rich_document.h"
#include "sapi/cli/presentation/messages.h"
#include "util/array.h"
#include "util/base64.h"
#include "util/cleanup.h"
#include "util/sha256.h"
#include "util/utf8.h"
#include <morph/tui/media.h>
#include <morph/tui/widgets/image.h>
#include <morph/tui/widgets/text_view.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cli_rich_document_image {
	struct morph_tui_media_asset *asset;
	struct morph_tui_view *view;
	morph_buf_t encoded;
	unsigned id, format, pixel_width, pixel_height;
	int x, y, columns, rows, placed;
	unsigned char digest[SHA256_DIGEST_SIZE];
};

struct cli_rich_document_images {
	morph_array_t items;
};

struct cli_rich_document {
	struct morph_tui_widget base;
	struct morph_tui_builder *builder;
	struct morph_tui_view *text, *media;
	struct cli_rich_document_images images;
	int styled;
};

static void cli_rich_document_images_cleanup(struct cli_rich_document_images *images)
{
	struct cli_rich_document_image *image;
	morph_array_foreach (image, &images->items, struct cli_rich_document_image) {
		morph_tui_media_asset_destroy(image->asset);
		morph_buf_cleanup(&image->encoded);
	}
	morph_array_cleanup(&images->items);
}

static int cli_rich_document_measure(struct morph_tui_view *view, int width,
	struct morph_tui_view_size *size)
{
	return morph_tui_view_measure(((struct cli_rich_document *)view)->text, width, size);
}

static int cli_rich_document_layout(struct morph_tui_view *view)
{
	struct cli_rich_document *document = (struct cli_rich_document *)view;
	struct morph_tui_view_rect content = morph_tui_view_content_rect(view);
	int rc = morph_tui_view_arrange(document->text, content);
	if (!rc && document->media)
		rc = morph_tui_view_arrange(document->media, content);
	struct cli_rich_document_image *image;
	morph_array_foreach (image, &document->images.items, struct cli_rich_document_image) {
		if (rc || !image->view)
			continue;
		rc = morph_tui_view_arrange(image->view, (struct morph_tui_view_rect){
			image->x, image->y, image->columns, image->rows});
	}
	return rc;
}

static void cli_rich_document_destroy(struct morph_tui_view *view)
{
	cli_rich_document_images_cleanup(&((struct cli_rich_document *)view)->images);
}

static const struct morph_tui_view_ops cli_rich_document_ops = {
	.measure = cli_rich_document_measure, .layout = cli_rich_document_layout,
	.destroy = cli_rich_document_destroy,
};

static unsigned cli_rich_document_parameter(const char *header, size_t length, char key)
{
	for (size_t i = 0; i + 2 < length; i++) {
		if ((i && header[i - 1] != ',') || header[i] != key || header[i + 1] != '=')
			continue;
		unsigned value = 0;
		for (size_t n = i + 2; n < length && header[n] >= '0' && header[n] <= '9'; n++) {
			unsigned digit = (unsigned)(header[n] - '0');
			if (value > (UINT_MAX - digit) / 10u)
				return 0;
			value = value * 10u + digit;
		}
		return value;
	}
	return 0;
}

static int cli_rich_document_transfer(struct cli_rich_document *document,
	struct cli_rich_document_images *images, const char *header, size_t header_length,
	const char *payload, size_t length)
{
	unsigned id = cli_rich_document_parameter(header, header_length, 'i');
	if (id) {
		struct cli_rich_document_image *slot = morph_array_push_borrow(&images->items);
		if (!slot)
			return -ENOMEM;
		*slot = (struct cli_rich_document_image){0};
		slot->id = id;
		slot->format = cli_rich_document_parameter(header, header_length, 'f');
		slot->pixel_width = cli_rich_document_parameter(header, header_length, 's');
		slot->pixel_height = cli_rich_document_parameter(header, header_length, 'v');
		unsigned columns = cli_rich_document_parameter(header, header_length, 'c');
		unsigned rows = cli_rich_document_parameter(header, header_length, 'r');
		if (!columns || columns > INT_MAX || !rows || rows > INT_MAX)
			return -EINVAL;
		slot->columns = (int)columns;
		slot->rows = (int)rows;
	}
	if (!images->items.nelts)
		return -EINVAL;
	struct cli_rich_document_image *image =
		morph_array_borrow(&images->items, images->items.nelts - 1);
	if (image->encoded.len > MORPH_MEDIA_MAX_FILE_BYTES * 2u ||
	    length > MORPH_MEDIA_MAX_FILE_BYTES * 2u - image->encoded.len)
		return -EFBIG;
	int rc = morph_buf_append(&image->encoded, payload, length);
	if (rc || cli_rich_document_parameter(header, header_length, 'm'))
		return rc;
	size_t decoded_length = 0;
	unsigned char *decoded MORPH_CLEANUP(morph_ucharp_cleanup) =
		base64_decode_copy(morph_buf_cstr_borrow(&image->encoded), &decoded_length);
	if (!decoded)
		return MORPH_ERR_FORMAT;
	if (decoded_length > MORPH_MEDIA_MAX_FILE_BYTES)
		return -EFBIG;
	struct sha256_ctx hash;
	sha256_init(&hash);
	sha256_update(&hash, decoded, decoded_length);
	sha256_final(&hash, image->digest);
	morph_buf_cleanup(&image->encoded);
	struct cli_rich_document_image *old;
	morph_array_foreach (old, &document->images.items, struct cli_rich_document_image) {
		if (old->asset && old->format == image->format &&
		    old->pixel_width == image->pixel_width &&
		    old->pixel_height == image->pixel_height &&
		    !memcmp(old->digest, image->digest, sizeof(image->digest))) {
			image->asset = morph_tui_media_asset_retain(old->asset);
			return 0;
		}
	}
	if (image->format == 100)
		return morph_tui_media_asset_create_png_copy(&image->asset, decoded,
			decoded_length);
	if (image->format != 32 || !image->pixel_width || !image->pixel_height ||
	    image->pixel_width > INT_MAX || image->pixel_height > INT_MAX ||
	    (size_t)image->pixel_width > decoded_length / 4u / image->pixel_height ||
	    (size_t)image->pixel_width * image->pixel_height * 4u != decoded_length)
		return MORPH_ERR_FORMAT;
	return morph_tui_media_asset_create_rgba_copy(&image->asset, decoded,
		(int)image->pixel_width, (int)image->pixel_height);
}

static int cli_rich_document_parse(struct cli_rich_document *document,
	struct cli_rich_document_images *images, morph_buf_t *text, const char *source)
{
	morph_buf_t style MORPH_CLEANUP(morph_buf_cleanup) = {0};
	morph_buf_t saved MORPH_CLEANUP(morph_buf_cleanup) = {0};
	size_t length = strlen(source);
	unsigned foreground = 0;
	int x = 0, y = 0, placeholder = 0;
	for (size_t i = 0; i < length;) {
		int rc = 0;
		if (source[i] == '\033' && i + 1 < length) {
			if (source[i + 1] == '_' && i + 2 < length && source[i + 2] == 'G') {
				const char *end = strstr(source + i + 3, "\033\\");
				if (!end)
					return -EAGAIN;
				const char *separator = memchr(source + i + 3, ';',
					(size_t)(end - (source + i + 3)));
				if (!separator)
					return MORPH_ERR_FORMAT;
				rc = cli_rich_document_transfer(document, images, source + i + 3,
					(size_t)(separator - (source + i + 3)), separator + 1,
					(size_t)(end - separator - 1));
				i = (size_t)(end - source) + 2;
			} else if (source[i + 1] == '7') {
				morph_buf_reset(&saved);
				rc = morph_buf_puts(&saved, style.data ? style.data : "\033[0m");
				i += 2;
			} else if (source[i + 1] == '8') {
				rc = morph_buf_puts(text, saved.data ? saved.data : "\033[0m");
				morph_buf_reset(&style);
				if (!rc)
					rc = morph_buf_puts(&style,
						saved.data ? saved.data : "\033[0m");
				i += 2;
			} else if (source[i + 1] == '[') {
				size_t end = i + 2;
				while (end < length &&
				       ((unsigned char)source[end] < 0x40u ||
					(unsigned char)source[end] > 0x7eu))
					end++;
				if (end == length)
					return -EAGAIN;
				if (source[end] == 'm') {
					unsigned r, g, b;
					if ((sscanf(source + i,
						"\033[38;2;%u;%u;%um", &r, &g, &b) == 3 ||
					     sscanf(source + i,
						"\033[38:2:%u:%u:%um", &r, &g, &b) == 3) &&
					    r <= 255u && g <= 255u && b <= 255u)
						foreground = r * 65536u + g * 256u + b;
					rc = morph_buf_append(text, source + i, end - i + 1);
					if (end - i == 3 && !memcmp(source + i, "\033[0m", 4))
						morph_buf_reset(&style);
					if (!rc)
						rc = morph_buf_append(&style, source + i,
							end - i + 1);
				}
				i = end + 1;
			} else if (source[i + 1] == ']') {
				size_t end = i + 2;
				while (end < length && source[end] != '\a' &&
				       !(source[end] == '\033' && end + 1 < length &&
					 source[end + 1] == '\\'))
					end++;
				if (end == length)
					return -EAGAIN;
				end += source[end] == '\a' ? 1 : 2;
				rc = morph_buf_append(text, source + i, end - i);
				i = end;
			} else
				i += 2;
			if (rc)
				return rc;
			continue;
		}
		unsigned cp;
		size_t bytes;
		if (!utf8_decode_codepoint(source + i, length - i, &cp, &bytes))
			return MORPH_ERR_FORMAT;
		if (cp == 0x10eeeeu) {
			struct cli_rich_document_image *image;
			int found = 0;
			morph_array_foreach (image, &images->items,
				struct cli_rich_document_image) {
				if ((image->id & 0xffffffu) == foreground) {
					if (!image->placed) {
						image->x = x;
						image->y = y;
						image->placed = 1;
					}
					found = 1;
					break;
				}
			}
			rc = found ? morph_buf_putc(text, ' ') :
				morph_buf_append(text, source + i, bytes);
			x++;
			placeholder = found;
		} else if (placeholder && utf8_classify_codepoint(cp) == UTF8_CLASS_MARK) {
			/* The framework media widget owns placeholder generation. */
		} else {
			placeholder = 0;
			rc = morph_buf_append(text, source + i, bytes);
			if (cp == '\n') {
				x = 0;
				y++;
			} else if (cp != '\r')
				x += utf8_codepoint_width(cp) > 0 ? utf8_codepoint_width(cp) : 0;
		}
		if (rc)
			return rc;
		i += bytes;
	}
	return 0;
}

static int cli_rich_document_leading_rows(const char *text)
{
	int rows = 0;
	for (size_t i = 0; text[i];) {
		if (text[i] == '\033' && text[i + 1] == '[') {
			i += 2;
			while (text[i] && ((unsigned char)text[i] < 0x40u ||
				(unsigned char)text[i] > 0x7eu))
				i++;
			if (text[i])
				i++;
		} else if (text[i] == '\n' || text[i] == '\r') {
			rows += text[i++] == '\n';
		} else
			break;
	}
	return rows;
}

int cli_rich_document_set_rendered_copy(struct morph_tui_view *view, const char *source, int styled)
{
	if (!view || view->ops != &cli_rich_document_ops || !source)
		return -EINVAL;
	if (!morph_tui_view_is_owner(view))
		return -EPERM;
	struct cli_rich_document *document = (struct cli_rich_document *)view;
	struct cli_rich_document_images
		images MORPH_CLEANUP(cli_rich_document_images_cleanup) = {0};
	struct cli_rich_document_image image;
	morph_buf_t text MORPH_CLEANUP(morph_buf_cleanup) = {0};
	int rc = morph_array_init(&images.items, 4, sizeof(image));
	if (!rc)
		rc = morph_buf_init(&text, 256);
	if (!rc)
		rc = cli_rich_document_parse(document, &images, &text, source);
	if (rc)
		return rc;
	/* Keep text controls stable: replacing each one would scan the owning
	 * controller's entire View scope once per restored message. */
	rc = cli_messages_set_rendered_view(document->text, morph_buf_cstr_borrow(&text), styled);
	if (rc)
		return rc;
	if (document->media && morph_tui_view_child_count(document->media)) {
		rc = morph_tui_view_clear_children(document->media);
		if (rc)
			return rc;
		struct cli_rich_document_image *old;
		morph_array_foreach (old, &document->images.items, struct cli_rich_document_image)
			old->view = NULL;
	}
	struct cli_rich_document_image *entry;
	int leading = cli_rich_document_leading_rows(morph_buf_cstr_borrow(&text));
	morph_array_foreach (entry, &images.items, struct cli_rich_document_image) {
		if (!entry->asset || !entry->placed)
			continue;
		if (!document->media) {
			struct morph_tui_widget *media = morph_tui_widget_add_borrow(
				document->builder, view, sizeof(*media), NULL, NULL);
			if (!media)
				return morph_tui_builder_error(document->builder);
			document->media = &media->view;
		}
		const struct morph_tui_view_options options = {
			.width = entry->columns, .height = entry->rows};
		entry->y = entry->y > leading ? entry->y - leading : 0;
		entry->view = morph_tui_widget_image_add_borrow(document->builder, document->media,
			entry->asset, &options);
	}
	if (document->media) {
		rc = morph_tui_view_set_hidden(document->media,
			!morph_tui_view_child_count(document->media));
		if (rc)
			return rc;
	}
	cli_rich_document_images_cleanup(&document->images);
	rc = morph_array_take(&document->images.items, &images.items);
	if (!rc)
		morph_tui_view_invalidate(view, 1);
	return rc ? rc : morph_tui_builder_error(document->builder);
}

struct morph_tui_view *cli_rich_document_add_borrow(struct morph_tui_builder *builder,
	struct morph_tui_view *parent)
{
	struct cli_rich_document *document =
		(struct cli_rich_document *)morph_tui_widget_add_borrow(
			builder, parent, sizeof(*document), &cli_rich_document_ops, NULL);
	if (!document)
		return NULL;
	document->builder = builder;
	document->text = morph_tui_widget_text_view_add_borrow(builder,
		&document->base.view, "", NULL);
	if (document->text) {
		morph_tui_widget_text_view_set_content_sizing(document->text, 1);
		morph_tui_view_set_focusable(document->text, 0);
	}
	return &document->base.view;
}
