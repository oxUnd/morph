#include "agent/history_internal.h"
#include "models/llm/stream.h"
#include "http/client.h"
#include "nb/cleanup.h"
#include "nb/mem.h"
#include "nb/file.h"
#include <morph/tui/media.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bench_allocator {
	size_t allocations;
	size_t resizes;
	size_t live;
	size_t peak;
};
static void bench_account(struct bench_allocator *stats, size_t before, size_t after)
{
	stats->live -= before;
	stats->live += after;
	if (stats->live > stats->peak)
		stats->peak = stats->live;
}
static void *bench_allocate(void *opaque, size_t size)
{
	struct bench_allocator *stats = opaque;
	void *ptr = malloc(size);
	stats->allocations++;
	if (ptr)
		bench_account(stats, 0, size);
	return ptr;
}
static void *bench_resize(void *opaque, void *ptr, size_t old_size, size_t new_size)
{
	struct bench_allocator *stats = opaque;
	size_t before = ptr ? old_size : 0;
	void *resized = realloc(ptr, new_size);
	stats->resizes++;
	if (resized)
		bench_account(stats, before, new_size);
	return resized;
}
static void bench_release(void *opaque, void *ptr, size_t size)
{
	struct bench_allocator *stats = opaque;
	if (ptr)
		bench_account(stats, size, 0);
	free(ptr);
}

static int bench_redaction(struct bench_allocator *stats)
{
	const char *prefix = "data:image/png;base64,";
	size_t length = 16u * 1024u * 1024u;
	char *source NB_CLEANUP(nb_charp_cleanup) = malloc(length + 1);
	if (!source)
		return NB_ERR_NOMEM;
	memset(source, 'A', length);
	memcpy(source, prefix, strlen(prefix));
	source[length] = '\0';
	char *output NB_CLEANUP(nb_charp_cleanup) = history_redacted_text_create(NULL, source);
	if (!output || strcmp(output, "[binary data omitted]"))
		return NB_ERR_STATE;
	printf("redact_16m,peak_native_bytes=%zu,output_bytes=%zu\n", stats->peak, strlen(output));
	return NB_OK;
}

static int bench_streams(struct bench_allocator *stats)
{
	struct llm_stream_ctx streams[128];
	for (size_t i = 0; i < sizeof(streams) / sizeof(streams[0]); i++)
		llm_stream_init(&streams[i], NULL, NULL);
	printf("128_idle_streams,live_native_bytes=%zu,alloc_calls=%zu\n",
	       stats->live, stats->allocations);
	for (size_t i = 0; i < sizeof(streams) / sizeof(streams[0]); i++)
		llm_stream_cleanup(&streams[i]);
	return NB_OK;
}

static int bench_http(struct bench_allocator *stats)
{
	struct http_session session NB_CLEANUP(http_session_cleanup) = {0};
	if (!nb_buf_reserve(&session.resp_body, 16u * 1024u * 1024u) ||
	    !nb_buf_append_cstr(&session.resp_body, "response"))
		return NB_ERR_NOMEM;
	size_t before = stats->live;
	http_session_reset(&session);
	printf("http_reset_after_16m,before_native_bytes=%zu,after_native_bytes=%zu\n",
	       before, stats->live);
	if (!nb_buf_empty(&session.resp_body) ||
	    !nb_buf_append_cstr(&session.resp_body, "next") ||
	    strcmp(nb_buf_cstr(&session.resp_body), "next"))
		return NB_ERR_STATE;
	return NB_OK;
}

static int bench_png(const char *path)
{
	nb_buf input NB_CLEANUP(nb_buf_destroy) = {0};
	int rc = nb_file_read_all(path, &input, 0, NULL);
	if (rc != NB_OK)
		return 1;
	struct morph_tui_media_asset *asset = NULL;
	rc = morph_tui_media_asset_create_png_copy(&asset,
		(const unsigned char *)nb_buf_cstr(&input), nb_buf_len(&input));
	nb_buf_destroy(&input);
	if (rc != NB_OK)
		return 1;
	printf("png_asset,width=%d,height=%d,first_channel=%u\n",
	       morph_tui_media_asset_width(asset), morph_tui_media_asset_height(asset),
	       (unsigned)morph_tui_media_asset_pixels_borrow(asset)[0]);
	morph_tui_media_asset_destroy(asset);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2)
		return bench_png(argv[1]);
	if (argc != 1)
		return 1;
	struct bench_allocator stats = {0};
	nb_allocator allocator = {bench_allocate, bench_resize, bench_release, &stats};
	const nb_allocator *previous = nb_mem_allocator();
	int (*cases[])(struct bench_allocator *) = {bench_redaction, bench_streams, bench_http};
	int rc = NB_OK;

	/* Install only in this isolated benchmark, before creating any native owner. */
	nb_mem_set_allocator(&allocator);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		stats = (struct bench_allocator){0};
		rc = cases[i](&stats);
		if (rc != NB_OK || stats.live != 0) {
			fprintf(stderr, "case %zu: status=%d, remaining_bytes=%zu\n",
				i, rc, stats.live);
			rc = NB_ERR_STATE;
			break;
		}
	}
	if (stats.live == 0)
		nb_mem_set_allocator(previous);
	return rc == NB_OK ? 0 : 1;
}
