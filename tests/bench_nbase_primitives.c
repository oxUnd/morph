/* Mechanism microbenchmark. Build from the repository root after CMake:
 * cc -O2 -Ivendor/nothing/include tests/bench_nbase_primitives.c \
 *    build/vendor/nothing/libnbase.a -o /tmp/bench-nbase-migration
 * Requested allocator capacity is measured here, not process RSS.
 */
#include "nb/buf.h"
#include "nb/cleanup.h"
#include "nb/mem.h"
#include "nb/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct allocation_stats {
	size_t changes;
	size_t current;
	size_t peak;
};
static void *count_alloc(void *ctx, size_t size)
{
	struct allocation_stats *stats = ctx;
	void *p = malloc(size);
	if (p) {
		stats->changes++;
		stats->current += size;
		if (stats->current > stats->peak)
			stats->peak = stats->current;
	}
	return p;
}
static void *count_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size)
{
	struct allocation_stats *stats = ctx;
	void *p = realloc(ptr, new_size);
	if (p) {
		stats->changes++;
		stats->current = stats->current - old_size + new_size;
		if (stats->current > stats->peak)
			stats->peak = stats->current;
	}
	return p;
}
static void count_free(void *ctx, void *ptr, size_t size)
{
	struct allocation_stats *stats = ctx;
	stats->current -= size;
	free(ptr);
}
int main(void)
{
	const size_t count = 50000;
	const char *fragment = "0123456789abcdef";
	struct allocation_stats stats = {0};
	nb_allocator allocator = {count_alloc, count_realloc, count_free, &stats};
	nb_mem_set_allocator(&allocator);
	int64_t started = nb_time_monotonic_ns();
	char *legacy NB_CLEANUP(nb_charp_cleanup) = NULL;
	for (size_t i = 0; i < count; i++) {
		size_t old = legacy ? strlen(legacy) : 0;
		char *next = realloc(legacy, old + 17);
		if (!next)
			return EXIT_FAILURE;
		legacy = next;
		memcpy(legacy + old, fragment, 17);
	}
	printf("arguments legacy: %.3f ms; reallocations=%zu; retained_bytes=%zu\n",
	       (double)(nb_time_monotonic_ns() - started) / 1e6, count, count * 16 + 1);
	free(legacy);
	legacy = NULL;
	started = nb_time_monotonic_ns();
	nb_buf buffer NB_CLEANUP(nb_buf_cleanup) = {0};
	nb_buf_init(&buffer);
	for (size_t i = 0; i < count; i++) {
		if (!nb_buf_append_cstr(&buffer, fragment))
			return EXIT_FAILURE;
	}
	printf("arguments nbase: %.3f ms; allocator_changes=%zu; peak_requested_bytes=%zu\n",
	       (double)(nb_time_monotonic_ns() - started) / 1e6, stats.changes, stats.peak);
	nb_buf_destroy(&buffer);
	const size_t lines = 30000;
	nb_buf_init(&buffer);
	for (size_t i = 0; i < lines; i++)
		nb_buf_append_cstr(&buffer, "data: fragment\n");
	size_t size = nb_buf_len(&buffer);
	char *bytes NB_CLEANUP(nb_charp_cleanup) = malloc(size + 1);
	if (!bytes)
		return EXIT_FAILURE;
	memcpy(bytes, nb_buf_cstr(&buffer), size + 1);
	started = nb_time_monotonic_ns();
	size_t rest = size;
	size_t moved = 0;
	size_t seen = 0;
	while (rest) {
		char *newline = memchr(bytes, '\n', rest);
		if (!newline)
			return EXIT_FAILURE;
		size_t consumed = (size_t)(newline - bytes) + 1;
		rest -= consumed;
		memmove(bytes, bytes + consumed, rest);
		moved += rest;
		seen++;
	}
	printf("lines legacy: %.3f ms; lines=%zu; shifted_bytes=%zu\n",
	       (double)(nb_time_monotonic_ns() - started) / 1e6, seen, moved);
	free(bytes);
	bytes = NULL;
	started = nb_time_monotonic_ns();
	nb_str line;
	seen = 0;
	while (nb_buf_next_line(&buffer, &line))
		seen++;
	printf("lines nbase: %.3f ms; lines=%zu; cursor_consumed_bytes=%zu\n",
	       (double)(nb_time_monotonic_ns() - started) / 1e6, seen, buffer.off);
	nb_buf_destroy(&buffer);
	printf("allocator_remaining_bytes=%zu\n", stats.current);
	nb_mem_set_allocator(NULL);
	return stats.current ? EXIT_FAILURE : EXIT_SUCCESS;
}
