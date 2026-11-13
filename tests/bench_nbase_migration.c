#include "models/llm/stream.h"
#include "http/sse.h"
#include "nb/cleanup.h"
#include "nb/file.h"
#include "nb/map.h"
#include "nb/mem.h"
#include "nb/time.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bench_allocator {
	size_t allocations;
	size_t resizes;
	size_t live;
	size_t peak;
};
struct bench_result {
	const char *name;
	struct bench_allocator *allocator;
	uint64_t units;
	uint64_t bytes;
	int64_t started;
	int64_t elapsed;
	size_t allocations;
	size_t resizes;
	size_t baseline;
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
static void bench_begin(struct bench_result *result, uint64_t units, uint64_t bytes)
{
	struct bench_allocator *stats = result->allocator;
	result->units = units;
	result->bytes = bytes;
	result->allocations = stats->allocations;
	result->resizes = stats->resizes;
	result->baseline = stats->live;
	stats->peak = stats->live;
	result->started = nb_time_monotonic_ns();
}
static void bench_end(struct bench_result *result)
{
	struct bench_allocator *stats = result->allocator;
	result->elapsed = nb_time_monotonic_ns() - result->started;
	result->allocations = stats->allocations - result->allocations;
	result->resizes = stats->resizes - result->resizes;
	result->peak = stats->peak;
}
static nb_status bench_model_deltas(struct bench_result *result)
{
	struct llm_stream_ctx stream NB_CLEANUP(llm_stream_cleanup) = {0};
	const char *delta = "0123456789abcdef";
	const int calls = 8;
	const size_t fragments = 32768;
	bench_begin(result, (uint64_t)calls * fragments,
		    (uint64_t)calls * fragments * strlen(delta));
	llm_stream_init(&stream, NULL, NULL);
	for (int i = 0; i < calls; i++) {
		struct tool_call *call = llm_stream_tool_call_ensure_borrow(&stream, i);
		if (!call)
			return NB_ERR_NOMEM;
		for (size_t j = 0; j < fragments; j++) {
			int status = llm_stream_call_input_append(&stream, call, delta);
			if (status != NB_OK)
				return (nb_status)status;
		}
		if (strlen(call->arguments) != fragments * strlen(delta))
			return NB_ERR_STATE;
	}
	bench_end(result);
	return NB_OK;
}
static int bench_sse_event(const char *event, const char *data, void *opaque)
{
	uint64_t *received = opaque;
	if (strcmp(event, "tool") != 0 || strcmp(data, "{\"delta\":\"0123456789\"}") != 0)
		return NB_ERR_STATE;
	(*received)++;
	return NB_OK;
}
static void bench_sse_cleanup(struct sse_parser *parser)
{
	sse_parser_free(parser);
}
static nb_status bench_sse_chunks(struct bench_result *result)
{
	struct sse_parser parser NB_CLEANUP(bench_sse_cleanup) = {0};
	const char *frame = "event: tool\r\ndata: {\"delta\":\"0123456789\"}\r\n\r\n";
	const size_t frame_bytes = strlen(frame);
	const uint64_t frames = 200000;
	uint64_t received = 0;
	bench_begin(result, frames, frames * frame_bytes);
	sse_parser_init(&parser, bench_sse_event, &received);
	for (uint64_t i = 0; i < frames; i++) {
		for (size_t at = 0; at < frame_bytes; at += 17) {
			size_t size = frame_bytes - at < 17 ? frame_bytes - at : 17;
			int status = sse_parser_feed(&parser, frame + at, size);
			if (status != NB_OK)
				return (nb_status)status;
		}
	}
	if (received != frames)
		return NB_ERR_STATE;
	bench_end(result);
	return NB_OK;
}
static nb_status bench_mcp_lines(struct bench_result *result)
{
	nb_buf pending NB_CLEANUP(nb_buf_cleanup) = {0};
	const char *frame = "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":\"0123456789\"}\n";
	const size_t frame_bytes = strlen(frame);
	const uint64_t frames = 200000;
	uint64_t received = 0;
	bench_begin(result, frames, frames * frame_bytes);
	for (uint64_t i = 0; i < frames; i++) {
		for (size_t at = 0; at < frame_bytes; at += 7) {
			size_t size = frame_bytes - at < 7 ? frame_bytes - at : 7;
			if (!nb_buf_append(&pending, frame + at, size))
				return NB_ERR_NOMEM;
			nb_str line;
			while (nb_buf_next_line(&pending, &line)) {
				if (line.len != frame_bytes - 1 ||
				    memcmp(line.ptr, frame, line.len))
					return NB_ERR_STATE;
				received++;
			}
		}
	}
	if (received != frames || !nb_buf_empty(&pending))
		return NB_ERR_STATE;
	bench_end(result);
	return NB_OK;
}
struct bench_directory {
	nb_buf path;
};
static void bench_directory_cleanup(struct bench_directory *directory)
{
	if (directory->path.ptr)
		(void)nb_file_remove_all(nb_buf_cstr(&directory->path), 0, NULL);
	nb_buf_destroy(&directory->path);
}
static nb_status bench_directory_copy(struct bench_result *result)
{
	struct bench_directory directory NB_CLEANUP(bench_directory_cleanup) = {0};
	nb_buf source NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf target NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf path NB_CLEANUP(nb_buf_cleanup) = {0};
	unsigned char chunk[64u * 1024u];
	const size_t files = 8;
	const size_t chunks = 128;
	nb_status status = nb_file_temp_dir(&directory.path, NULL, NULL, NULL);
	if (status != NB_OK)
		return status;
	if (!nb_buf_printf(&source, "%s/source", nb_buf_cstr(&directory.path)) ||
	    !nb_buf_printf(&target, "%s/copied", nb_buf_cstr(&directory.path)))
		return NB_ERR_NOMEM;
	status = nb_file_mkdir(nb_buf_cstr(&source), false, NULL);
	if (status != NB_OK)
		return status;
	memset(chunk, 0x5a, sizeof(chunk));
	for (size_t i = 0; i < files; i++) {
		nb_buf_clear(&path);
		if (!nb_buf_printf(&path, "%s/file-%zu.bin", nb_buf_cstr(&source), i))
			return NB_ERR_NOMEM;
		for (size_t j = 0; j < chunks; j++) {
			status = nb_file_append(nb_buf_cstr(&path), chunk, sizeof(chunk), NULL);
			if (status != NB_OK)
				return status;
		}
	}
	bench_begin(result, files, (uint64_t)files * chunks * sizeof(chunk));
	nb_file_copy_options options = {.recursive = true, .nofollow_source = true};
	status = nb_file_copy(nb_buf_cstr(&source), nb_buf_cstr(&target), &options, NULL);
	bench_end(result);
	if (status != NB_OK)
		return status;
	for (size_t i = 0; i < files; i++) {
		uint64_t size = 0;
		nb_buf_clear(&path);
		if (!nb_buf_printf(&path, "%s/file-%zu.bin", nb_buf_cstr(&target), i))
			return NB_ERR_NOMEM;
		status = nb_file_size(nb_buf_cstr(&path), &size, NULL);
		if (status != NB_OK || size != chunks * sizeof(chunk))
			return status != NB_OK ? status : NB_ERR_STATE;
	}
	return NB_OK;
}
struct bench_item {
	size_t index;
	size_t generation;
};
static nb_status bench_container_cycles(struct bench_result *result)
{
	nb_vec items NB_CLEANUP(nb_vec_cleanup) = {0};
	nb_map *map NB_CLEANUP(nb_map_cleanup) = NULL;
	nb_buf key NB_CLEANUP(nb_buf_cleanup) = {0};
	const size_t count = 4096;
	const size_t cycles = 100;
	struct bench_item item = {0};
	bench_begin(result, count * cycles, 0);
	nb_vec_init(&items, sizeof(item));
	if (!nb_vec_reserve(&items, count))
		return NB_ERR_NOMEM;
	map = nb_map_new_with_cap(count * 2);
	if (!map)
		return NB_ERR_NOMEM;
	for (size_t cycle = 0; cycle < cycles; cycle++) {
		for (size_t i = 0; i < count; i++) {
			item.index = i;
			item.generation = cycle;
			if (!nb_vec_push_copy(&items, &item))
				return NB_ERR_NOMEM;
			nb_buf_clear(&key);
			if (!nb_buf_printf(&key, "item-%zu", i) ||
			    !nb_map_put(map, nb_buf_str(&key), nb_vec_at(&items, i), NULL))
				return NB_ERR_NOMEM;
		}
		for (size_t i = 0; i < count; i++) {
			nb_buf_clear(&key);
			if (!nb_buf_printf(&key, "item-%zu", i))
				return NB_ERR_NOMEM;
			struct bench_item *received = nb_map_del(map, nb_buf_str(&key));
			if (!received || received->index != i || received->generation != cycle)
				return NB_ERR_STATE;
		}
		nb_vec_clear(&items);
		if (nb_map_size(map) != 0)
			return NB_ERR_STATE;
	}
	bench_end(result);
	return NB_OK;
}
int main(void)
{
	struct bench_allocator stats = {0};
	nb_allocator allocator = {bench_allocate, bench_resize, bench_release, &stats};
	const nb_allocator *previous = nb_mem_allocator();
	const struct {
		const char *name;
		nb_status (*run)(struct bench_result *);
	} cases[] = {{"model_argument_deltas", bench_model_deltas},
		     {"sse_chunk_parser", bench_sse_chunks},
		     {"mcp_line_cursor", bench_mcp_lines},
		     {"directory_stream_copy", bench_directory_copy},
		     {"long_lived_vec_map", bench_container_cycles}};
	int exit_status = 0;
	/* Install once before any nbase object; every case releases all owners.
	 * This executable never starts a runtime or changes its allocator. */
	nb_mem_set_allocator(&allocator);
	puts("case,units,bytes,elapsed_ms,alloc_calls,realloc_calls,peak_live_bytes,"
	     "baseline_live_bytes,live_bytes_after");
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct bench_result result = {.name = cases[i].name, .allocator = &stats};
		nb_status status = cases[i].run(&result);
		if (status != NB_OK || stats.live != 0) {
			fprintf(stderr, "%s: %s; tracked bytes remaining=%zu\n", cases[i].name,
				nb_status_str(status), stats.live);
			exit_status = 1;
			break;
		}
		printf("%s,%" PRIu64 ",%" PRIu64 ",%.3f,%zu,%zu,%zu,%zu,%zu\n", result.name,
		       result.units, result.bytes, (double)result.elapsed / 1000000.0,
		       result.allocations, result.resizes, result.peak, result.baseline,
		       stats.live);
	}
	/* All lexical cleanup has run before restoring the allocator. */
	if (stats.live == 0)
		nb_mem_set_allocator(previous);
	return exit_status;
}
