#include "agent/history_internal.h"
#include "models/llm/stream.h"
#include "http/request.h"
#include "sapi/cli/adapters/media/asset.h"
#include <nb/cleanup.h>
#include <nb/mem.h>
#include <nb/file.h>
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
	nb_http_client *client NB_CLEANUP(http_client_cleanup) = NULL;
	nb_http_call *call NB_CLEANUP(http_call_cleanup) = NULL;
	nb_buf payload NB_CLEANUP(nb_buf_cleanup) = {0};
	char chunk[BUFSIZ] = {0};
	for (size_t bytes = 0; bytes < 16u * 1024u * 1024u; bytes += sizeof(chunk)) {
		if (!nb_buf_append(&payload, chunk, sizeof(chunk)))
			return NB_ERR_NOMEM;
	}

	int rc = http_client_open(NULL, &client);
	nb_http_request request;
	http_request_init(&request, "POST", "http://127.0.0.1:1/");
	request.body = nb_buf_str(&payload);
	if (rc == NB_OK)
		rc = nb_http_client_send(client, &request, NULL, &call, NULL);

	if (rc != NB_OK)
		return rc;

	nb_buf_cleanup(&payload);
	size_t before = stats->live;

	http_call_cleanup(&call);
	printf("http_call_release_after_16m,before_native_bytes=%zu,after_native_bytes=%zu\n",
	       before, stats->live);
	return stats->live < before && nb_http_client_pending(client) == 0 ? NB_OK : NB_ERR_STATE;
}

static void bench_asset_cleanup(struct nt_media_asset **asset)
{
	nt_media_asset_release(*asset);
}

static int bench_png(const char *path)
{
	nb_buf input NB_CLEANUP(nb_buf_destroy) = {0};
	int rc = nb_file_read_all(path, &input, 0, NULL);
	if (rc != NB_OK)
		return 1;

	struct nt_media_asset *asset NB_CLEANUP(bench_asset_cleanup) = NULL;
	rc = cli_media_asset_from_png(&asset, (const unsigned char *)nb_buf_cstr(&input),
				      nb_buf_len(&input));
	nb_buf_destroy(&input);
	if (rc != NB_OK)
		return 1;

	printf("png_asset,width=%d,height=%d,first_channel=%u\n", nt_media_asset_width(asset),
	       nt_media_asset_height(asset), (unsigned)nt_media_asset_pixels(asset)[0]);
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
