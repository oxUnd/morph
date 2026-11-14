#include "native_media.h"
#include "nb/err.h"
#include <morph/tui/media.h>
#include <morph/tui/draw.h>
#include "nb/vec.h"
#include "nb/cleanup.h"
#include "nb/encoding.h"
#include "media/image.h"
#include "media/image.h"
#include "nb/random.h"
#include "stb_image_resize2.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum native_media_protocol {
	NATIVE_MEDIA_NONE,
	NATIVE_MEDIA_KITTY,
	NATIVE_MEDIA_ITERM,
	NATIVE_MEDIA_SIXEL
};

struct native_media_key {
	uint64_t asset;
	int x, y, width, height;
};

struct native_media_snapshot {
	struct native_media_key key;
	int x, y, width, height;
};

struct native_media_cache {
	struct native_media_key key;
	nb_buf base64;
	nb_buf sixel;
	int sixel_width, sixel_height;
	size_t png_bytes;
	uint32_t image_id;
	int sent, placed;
	uint64_t generation;
};

struct render_native_media {
	nb_vec cache, previous;
	enum native_media_protocol protocol;
	FILE *output;
	uint64_t generation;
	uint32_t next_image_id;
	int invalid;
};

static enum native_media_protocol native_media_protocol_detect(void)
{
	const char *term = getenv("TERM");
	const char *program = getenv("TERM_PROGRAM");
	if (getenv("KITTY_WINDOW_ID") || (term && strstr(term, "kitty")) ||
	    (program && !strcmp(program, "ghostty")))
		return NATIVE_MEDIA_KITTY;
	if (getenv("ITERM_PROFILE") || (program && !strcmp(program, "iTerm.app")))
		return NATIVE_MEDIA_ITERM;
	if (term && strstr(term, "sixel"))
		return NATIVE_MEDIA_SIXEL;
	return NATIVE_MEDIA_NONE;
}

int morph_tui_media_protocol_available(void)
{
	return native_media_protocol_detect() != NATIVE_MEDIA_NONE;
}

static struct native_media_key native_media_placement_key(
	const struct morph_tui_media_placement *placement)
{
	return (struct native_media_key){morph_tui_media_asset_key(placement->asset),
		placement->source_x, placement->source_y,
		placement->source_width, placement->source_height};
}

static int native_media_key_equal(struct native_media_key a, struct native_media_key b)
{
	return a.asset == b.asset && a.x == b.x && a.y == b.y &&
	       a.width == b.width && a.height == b.height;
}

static int native_media_snapshot_equal(const struct native_media_snapshot *snapshot,
					const struct morph_tui_media_placement *placement)
{
	return native_media_key_equal(snapshot->key, native_media_placement_key(placement)) &&
	       snapshot->x == placement->x && snapshot->y == placement->y &&
	       snapshot->width == placement->width && snapshot->height == placement->height;
}

static void native_media_cache_cleanup(struct native_media_cache **owner)
{
	if (!owner || !*owner)
		return;
	nb_buf_destroy(&(*owner)->base64);
	nb_buf_cleanup(&(*owner)->sixel);
	free(*owner);
	*owner = NULL;
}

static struct native_media_cache *native_media_cache_steal(struct native_media_cache **owner)
{
	struct native_media_cache *value = *owner;
	*owner = NULL;
	return value;
}

void render_native_media_destroy(struct render_native_media *media)
{
	if (!media)
		return;
	nb_buf commands NB_CLEANUP(nb_buf_cleanup) = {0};
	for (size_t i = 0; i < media->cache.len; i++) {
		struct native_media_cache **slot = nb_vec_at(&media->cache, i);
		if (media->protocol == NATIVE_MEDIA_KITTY && media->output)
			(void)nb_buf_printf(&commands, "\033_Ga=d,d=I,i=%u,q=2;\033\\",
					    (*slot)->image_id);
		native_media_cache_cleanup(slot);
	}
	if (media->output && commands.len)
		(void)morph_tui_draw_write(media->output, &commands);
	nb_vec_cleanup(&media->cache);
	nb_vec_cleanup(&media->previous);
	free(media);
}

static void native_media_cleanup(struct render_native_media **owner)
{
	if (!owner)
		return;
	render_native_media_destroy(*owner);
	*owner = NULL;
}

static int native_media_create(struct render_native_media **out)
{
	struct render_native_media *media NB_CLEANUP(native_media_cleanup) =
		calloc(1, sizeof(*media));
	if (!media)
		return NB_ERR_NOMEM;
	struct native_media_cache *entry;
	struct native_media_snapshot snapshot;
	int rc = (nb_vec_init(&media->cache, sizeof(entry)),
		  nb_vec_reserve(&media->cache, 16) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_vec_init(&media->previous, sizeof(snapshot)),
		      nb_vec_reserve(&media->previous, 16) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = nb_random_bytes((unsigned char *)&media->next_image_id,
				     sizeof(media->next_image_id), NULL);
	if (rc)
		return rc;
	media->protocol = native_media_protocol_detect();
	media->invalid = 1;
	*out = media;
	media = NULL;
	return 0;
}

int render_native_media_prepare(struct render_native_media **owner,
				const struct morph_tui_surface *surface,
				struct morph_tui_frame *frame)
{
	if (!owner || !surface || !frame)
		return NB_ERR_INVAL;
	size_t count = morph_tui_surface_image_count(surface);
	if (!*owner && native_media_protocol_detect() == NATIVE_MEDIA_NONE)
		return 0;
	if (!*owner && !count)
		return 0;
	if (!*owner) {
		int rc = native_media_create(owner);
		if (rc)
			return rc;
	}
	struct render_native_media *media = *owner;
	int changed = media->invalid || count != media->previous.len;
	for (size_t i = 0; !changed && i < count; i++) {
		const struct native_media_snapshot *snapshot = nb_vec_at(&media->previous, i);
		changed = !native_media_snapshot_equal(snapshot,
			morph_tui_surface_image_borrow(surface, i));
	}
	if (changed)
		morph_tui_frame_invalidate(frame);
	return 0;
}

void render_native_media_invalidate(struct render_native_media *media)
{
	if (!media)
		return;
	media->invalid = 1;
	for (size_t i = 0; i < media->cache.len; i++) {
		struct native_media_cache **slot = nb_vec_at(&media->cache, i);
		(*slot)->sent = 0;
	}
}

int render_native_media_needs_refresh(const struct render_native_media *media,
	const struct morph_tui_surface *surface, const struct morph_tui_frame *frame,
	const char *rows)
{
	if (!media)
		return 0;
	if (media->invalid || !frame->rows)
		return 1;
	size_t count = morph_tui_surface_image_count(surface);
	const char *old = nb_buf_cstr(&frame->previous);
	for (int y = 0; y < frame->rows; y++) {
		size_t length = strcspn(rows, "\n"), previous = strcspn(old, "\n");
		if (length != previous || memcmp(rows, old, length)) {
			for (size_t i = 0; i < count; i++) {
				const struct morph_tui_media_placement *placement =
					morph_tui_surface_image_borrow(surface, i);
				if (y >= placement->y && y - placement->y < placement->height)
					return 1;
			}
		}
		rows += length + (rows[length] == '\n');
		old += previous + (old[previous] == '\n');
	}
	return 0;
}

static int native_media_cache_borrow(struct render_native_media *media,
				    const struct morph_tui_media_placement *placement,
				    struct native_media_cache **out)
{
	struct native_media_key key = native_media_placement_key(placement);
	for (size_t i = 0; i < media->cache.len; i++) {
		struct native_media_cache **slot = nb_vec_at(&media->cache, i);
		if (native_media_key_equal((*slot)->key, key)) {
			*out = *slot;
			return 0;
		}
	}
	struct native_media_cache *entry NB_CLEANUP(native_media_cache_cleanup) =
		calloc(1, sizeof(*entry));
	if (!entry)
		return NB_ERR_NOMEM;
	/* Sixel consumes pixels directly; PNG and Base64 are only used by Kitty/iTerm. */
	if (media->protocol != NATIVE_MEDIA_SIXEL) {
		nb_buf png NB_CLEANUP(nb_buf_destroy) = {0};
		int rc = morph_tui_media_asset_png_append(placement->asset, key.x, key.y,
							 key.width, key.height, &png);
		if (rc)
			return rc;
		rc = nb_base64_encode(png.ptr, png.len, NB_B64_STD, true, &entry->base64, NULL);
		if (rc != NB_OK)
			return rc;
		(void)nb_buf_shrink(&entry->base64);
		entry->png_bytes = png.len;
	}
	entry->key = key;
	if (!media->next_image_id)
		media->next_image_id++;
	entry->image_id = media->next_image_id++;
	struct native_media_cache **slot = nb_vec_push(&media->cache);
	if (!slot)
		return NB_ERR_NOMEM;
	*slot = native_media_cache_steal(&entry);
	*out = *slot;
	return 0;
}

static int native_media_kitty_transmit(struct native_media_cache *entry, nb_buf *out)
{
	size_t length = nb_buf_len(&entry->base64);
	for (size_t offset = 0; offset < length; offset += 4096) {
		size_t chunk = length - offset < 4096 ? length - offset : 4096;
		int more = offset + chunk < length;
		int rc = offset ? (nb_buf_printf(out, "\033_Gm=%d;", more) ? 0 : NB_ERR_NOMEM)
				: (nb_buf_printf(out, "\033_Ga=t,f=100,i=%u,q=2,m=%d;",
						 entry->image_id, more)
					   ? 0
					   : NB_ERR_NOMEM);
		if (!rc)
			rc = nb_buf_append(out, nb_buf_cstr(&entry->base64) + offset, chunk)
				     ? 0 : NB_ERR_NOMEM;
		if (!rc)
			rc = (nb_buf_append_cstr(out, "\033\\") ? 0 : NB_ERR_NOMEM);
		if (rc)
			return rc;
	}
	entry->sent = 1;
	return 0;
}

static unsigned native_media_sixel_color(const unsigned char *pixel)
{
	unsigned red = ((unsigned)pixel[0] * 5u + 127u) / 255u;
	unsigned green = ((unsigned)pixel[1] * 5u + 127u) / 255u;
	unsigned blue = ((unsigned)pixel[2] * 5u + 127u) / 255u;
	return red * 36u + green * 6u + blue;
}

static unsigned native_media_sixel_mask(const unsigned char *pixels, int width, int height,
				       int x, int y, unsigned color)
{
	unsigned mask = 0;
	for (int bit = 0; bit < 6 && y + bit < height; bit++) {
		const unsigned char *pixel = pixels + ((size_t)(y + bit) * (size_t)width +
			(size_t)x) * 4u;
		if (pixel[3] >= 128 && native_media_sixel_color(pixel) == color)
			mask |= 1u << (unsigned)bit;
	}
	return mask;
}

static int native_media_sixel_run(nb_buf *out, unsigned mask, int count)
{
	if (count >= 4)
		return (nb_buf_printf(out, "!%d%c", count, (char)(63u + mask)) ? 0 : NB_ERR_NOMEM);
	int rc = 0;
	for (int i = 0; !rc && i < count; i++)
		rc = (nb_buf_append_char(out, (char)(63u + mask)) ? 0 : NB_ERR_NOMEM);
	return rc;
}

static int native_media_sixel_band(nb_buf *out, const unsigned char *pixels, int width, int height,
				   int y)
{
	unsigned char colors[216] = {0};
	for (int row = y; row < y + 6 && row < height; row++) {
		for (int x = 0; x < width; x++) {
			const unsigned char *pixel = pixels +
				((size_t)row * (size_t)width + (size_t)x) * 4u;
			if (pixel[3] >= 128)
				colors[native_media_sixel_color(pixel)] = 1;
		}
	}
	int rc = 0, started = 0;
	for (unsigned color = 0; !rc && color < 216; color++) {
		if (!colors[color])
			continue;
		if (started++)
			rc = (nb_buf_append_char(out, '$') ? 0 : NB_ERR_NOMEM);
		if (!rc)
			rc = (nb_buf_printf(out, "#%u", color) ? 0 : NB_ERR_NOMEM);
		unsigned last = 0;
		int count = 0;
		for (int x = 0; !rc && x < width; x++) {
			unsigned mask = native_media_sixel_mask(pixels, width, height, x, y, color);
			if (count && mask != last) {
				rc = native_media_sixel_run(out, last, count);
				count = 0;
			}
			last = mask;
			count++;
		}
		if (!rc)
			rc = native_media_sixel_run(out, last, count);
	}
	return rc;
}

static int native_media_sixel(const struct morph_tui_media_placement *placement, FILE *output,
			      nb_buf *out, struct native_media_cache *entry)
{
	struct winsize size = {0};
	int cell_width = 8, cell_height = 16;
	if (ioctl(fileno(output), TIOCGWINSZ, &size) == 0) {
		if (size.ws_col && size.ws_xpixel >= size.ws_col)
			cell_width = size.ws_xpixel / size.ws_col;
		if (size.ws_row && size.ws_ypixel >= size.ws_row)
			cell_height = size.ws_ypixel / size.ws_row;
	}
	int64_t target_width = (int64_t)placement->width * cell_width;
	int64_t target_height = (int64_t)placement->height * cell_height;
	if (target_width < 1 || target_height < 1 || target_width > 4096 || target_height > 4096)
		return NB_ERR_TOO_BIG;
	int width = (int)target_width, height = (int)target_height;
	if (entry->sixel.len && entry->sixel_width == width && entry->sixel_height == height)
		return (nb_buf_append(out, entry->sixel.ptr, entry->sixel.len) ? 0 : NB_ERR_NOMEM);
	nb_buf encoded NB_CLEANUP(nb_buf_cleanup) = {0};
	int source_width = morph_tui_media_asset_width(placement->asset);
	const unsigned char *source = morph_tui_media_asset_pixels_borrow(placement->asset) +
		((size_t)placement->source_y * (size_t)source_width +
		 (size_t)placement->source_x) * 4u;
	/* Sixel quantizes encoded RGBA channels directly. Keep this resampling
	 * in that channel space, including alpha, without the sRGB lookup path. */
	unsigned char *pixels NB_CLEANUP(nb_ucharp_cleanup) =
		stbir_resize_uint8_linear(source, placement->source_width, placement->source_height,
					  source_width * 4, NULL, width, height, 0, STBIR_RGBA);
	if (!pixels)
		return NB_ERR_NOMEM;
	int rc = (nb_buf_printf(&encoded, "\033P0;1;0q\"1;1;%d;%d", width, height) ? 0
										   : NB_ERR_NOMEM);
	for (unsigned color = 0; !rc && color < 216; color++)
		rc = (nb_buf_printf(&encoded, "#%u;2;%u;%u;%u", color, (color / 36u) * 20u,
				    (color / 6u % 6u) * 20u, (color % 6u) * 20u)
			      ? 0
			      : NB_ERR_NOMEM);
	for (int y = 0; !rc && y < height; y += 6) {
		rc = native_media_sixel_band(&encoded, pixels, width, height, y);
		if (!rc && y + 6 < height)
			rc = (nb_buf_append_char(&encoded, '-') ? 0 : NB_ERR_NOMEM);
	}
	if (!rc)
		rc = (nb_buf_append_cstr(&encoded, "\033\\") ? 0 : NB_ERR_NOMEM);
	if (rc)
		return rc;
	(nb_buf_destroy(&entry->sixel), nb_buf_move(&entry->sixel, &encoded));
	entry->sixel_width = width;
	entry->sixel_height = height;
	return (nb_buf_append(out, entry->sixel.ptr, entry->sixel.len) ? 0 : NB_ERR_NOMEM);
}

static int native_media_snapshot_append(nb_vec *array,
					const struct morph_tui_media_placement *placement)
{
	struct native_media_snapshot *snapshot = nb_vec_push(array);
	if (!snapshot)
		return NB_ERR_NOMEM;
	*snapshot = (struct native_media_snapshot){native_media_placement_key(placement),
		placement->x, placement->y, placement->width, placement->height};
	return 0;
}

static void native_media_trim(struct render_native_media *media, nb_buf *out)
{
	size_t bytes = 0;
	for (size_t i = 0; i < media->cache.len; i++) {
		struct native_media_cache **slot = nb_vec_at(&media->cache, i);
		bytes += (*slot)->base64.cap + (*slot)->sixel.cap;
	}
	for (size_t i = 0;
	     i < media->cache.len && (media->cache.len > 128 || bytes > 32u * 1024u * 1024u);) {
		struct native_media_cache **slot = nb_vec_at(&media->cache, i);
		if ((*slot)->generation == media->generation) {
			i++;
			continue;
		}
		if (media->protocol == NATIVE_MEDIA_KITTY)
			(void)nb_buf_printf(out, "\033_Ga=d,d=I,i=%u,q=2;\033\\",
					    (*slot)->image_id);
		bytes -= (*slot)->base64.cap + (*slot)->sixel.cap;
		native_media_cache_cleanup(slot);
		nb_vec_remove(&media->cache, i);
	}
}

int render_native_media_append(struct render_native_media *media,
			       const struct morph_tui_surface *surface, FILE *output,
			       nb_buf *commands)
{
	if (!media || media->protocol == NATIVE_MEDIA_NONE)
		return 0;
	if (!surface || !output || !commands)
		return NB_ERR_INVAL;
	media->output = output;
	media->generation++;
	nb_vec next NB_CLEANUP(nb_vec_cleanup) = {0};
	struct native_media_snapshot snapshot;
	int rc = (nb_vec_init(&next, sizeof(snapshot)),
		  nb_vec_reserve(&next, 16) ? 0 : NB_ERR_NOMEM);
	size_t count = morph_tui_surface_image_count(surface);
	if (!rc && (count || media->previous.len))
		rc = (nb_buf_append_cstr(commands, "\0337") ? 0 : NB_ERR_NOMEM);
	for (size_t i = 0; !rc && i < media->cache.len; i++) {
		struct native_media_cache **slot = nb_vec_at(&media->cache, i);
		if (media->protocol == NATIVE_MEDIA_KITTY && (*slot)->placed)
			rc = (nb_buf_printf(commands, "\033_Ga=d,d=i,i=%u,q=2;\033\\",
					    (*slot)->image_id)
				      ? 0
				      : NB_ERR_NOMEM);
		(*slot)->placed = 0;
	}
	for (size_t i = 0; !rc && i < count; i++) {
		const struct morph_tui_media_placement *placement =
			morph_tui_surface_image_borrow(surface, i);
		struct native_media_cache *entry = NULL;
		rc = native_media_cache_borrow(media, placement, &entry);
		if (rc)
			break;
		entry->generation = media->generation;
		if (media->protocol == NATIVE_MEDIA_KITTY && !entry->sent)
			rc = native_media_kitty_transmit(entry, commands);
		if (!rc)
			rc = (nb_buf_printf(commands, "\033[%d;%dH", placement->y + 1,
					    placement->x + 1)
				      ? 0
				      : NB_ERR_NOMEM);
		if (!rc && media->protocol == NATIVE_MEDIA_KITTY) {
			rc = (nb_buf_printf(
				      commands, "\033_Ga=p,i=%u,p=%zu,c=%d,r=%d,C=1,q=2;\033\\",
				      entry->image_id, i + 1, placement->width, placement->height)
				      ? 0
				      : NB_ERR_NOMEM);
			entry->placed = 1;
		} else if (!rc && media->protocol == NATIVE_MEDIA_ITERM)
			rc = (nb_buf_printf(commands,
					    "\033]1337;File=inline=1;size=%zu;width=%d;height=%d;"
					    "preserveAspectRatio=0:%s\a",
					    entry->png_bytes, placement->width, placement->height,
					    nb_buf_cstr(&entry->base64))
				      ? 0
				      : NB_ERR_NOMEM);
		else if (!rc)
			rc = native_media_sixel(placement, output, commands, entry);
		if (!rc)
			rc = native_media_snapshot_append(&next, placement);
	}
	if (!rc)
		native_media_trim(media, commands);
	if (!rc && commands->len)
		rc = (nb_buf_append_cstr(commands, "\0338") ? 0 : NB_ERR_NOMEM);
	if (rc) {
		render_native_media_invalidate(media);
		return rc;
	}
	nb_vec_cleanup(&media->previous);
	rc = (nb_vec_move(&media->previous, &next), 0);
	media->invalid = rc != 0;
	return rc;
}

int render_native_media_present(struct render_native_media *media,
				const struct morph_tui_surface *surface, FILE *output)
{
	nb_buf native NB_CLEANUP(nb_buf_cleanup) = {0};
	int rc = render_native_media_append(media, surface, output, &native);
	if (rc || !native.len)
		return rc;
	nb_buf commands NB_CLEANUP(nb_buf_cleanup) = {0};
	rc = (nb_buf_append_cstr(&commands, "\033[?2026h") ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_buf_append(&commands, native.ptr, native.len) ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = (nb_buf_append_cstr(&commands, "\033[?2026l") ? 0 : NB_ERR_NOMEM);
	if (!rc)
		rc = morph_tui_draw_write(output, &commands);
	if (rc)
		render_native_media_invalidate(media);
	return rc;
}
