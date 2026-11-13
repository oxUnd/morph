#include "nb/map.h"

#include "nb/mem.h"

#include <stdatomic.h>
#include <string.h>

#define NB_MAP_BIN_COUNT 11u
#define NB_MAP_SLAB_MIN_BYTES 1024u
#define NB_MAP_SLAB_MAX_BYTES (16u * 1024u)
#define NB_MAP_ALIGN 16u
#define NB_MAP_ALIGN_UP(n) \
    (((n) + (NB_MAP_ALIGN - 1u)) & ~((size_t)NB_MAP_ALIGN - 1u))

typedef struct nb_map_entry {
    struct nb_map_entry *bucket_next;
    struct nb_map_entry *order_prev;
    struct nb_map_entry *order_next;
    uint64_t hash;
    size_t klen;
    void *value;
    size_t alloc_size;
    char key[];
} nb_map_entry;

typedef struct nb_map_slab {
    struct nb_map_slab *next;
    size_t slot_size;
    size_t count;
} nb_map_slab;

#define NB_MAP_SLAB_HDR NB_MAP_ALIGN_UP(sizeof(nb_map_slab))

struct nb_map {
    nb_map_entry **buckets;
    size_t nbuckets;
    size_t size;
    nb_map_entry *head;
    nb_map_entry *tail;
    nb_map_value_dtor dtor;
    void *dtor_ctx;
    uint64_t seed;
    void *free_lists[NB_MAP_BIN_COUNT];
    nb_map_slab *slabs;
};

static _Atomic uint64_t g_map_seed = 1u;

static uint64_t map_seed_new(const void *address)
{
    uint64_t value =
        atomic_fetch_add_explicit(&g_map_seed, UINT64_C(0x9e3779b97f4a7c15),
                                  memory_order_relaxed);
    value ^= (uint64_t)(uintptr_t)address;
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

static uint64_t map_hash(const nb_map *map, nb_str key)
{
    const unsigned char *p = (const unsigned char *)key.ptr;
    uint64_t hash = UINT64_C(14695981039346656037) ^ map->seed;
    for (size_t i = 0; i < key.len; i++) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static size_t map_bin_size(size_t index)
{
    static const size_t sizes[NB_MAP_BIN_COUNT] = {
        64u, 80u, 96u, 112u, 128u, 192u, 256u, 512u, 1024u, 2048u, 4096u
    };
    return sizes[index];
}

static size_t map_bin_index(size_t size)
{
    for (size_t i = 0; i < NB_MAP_BIN_COUNT; i++) {
        if (size <= map_bin_size(i)) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t map_entry_size(size_t key_len)
{
    if (key_len > SIZE_MAX - sizeof(nb_map_entry) - 1u) {
        return 0;
    }
    return sizeof(nb_map_entry) + key_len + 1u;
}

static bool map_refill_bin(nb_map *map, size_t index)
{
    size_t slot_size = map_bin_size(index);
    size_t target_bytes =
        slot_size < 256u ? NB_MAP_SLAB_MIN_BYTES : slot_size * 8u;
    if (target_bytes > NB_MAP_SLAB_MAX_BYTES) {
        target_bytes = NB_MAP_SLAB_MAX_BYTES;
    }
    size_t count = target_bytes / slot_size;
    if (count == 0) {
        count = 1;
    }
    if (count > (SIZE_MAX - NB_MAP_SLAB_HDR) / slot_size) {
        return false;
    }
    size_t bytes = NB_MAP_SLAB_HDR + count * slot_size;
    nb_map_slab *slab = nb_try_alloc(bytes);
    if (!slab) {
        return false;
    }
    slab->next = map->slabs;
    slab->slot_size = slot_size;
    slab->count = count;
    map->slabs = slab;

    unsigned char *data = (unsigned char *)slab + NB_MAP_SLAB_HDR;
    for (size_t i = 0; i < count; i++) {
        void *slot = data + i * slot_size;
        *(void **)slot = map->free_lists[index];
        map->free_lists[index] = slot;
    }
    return true;
}

static nb_map_entry *map_entry_alloc(nb_map *map, size_t size)
{
    size_t index = map_bin_index(size);
    if (index == SIZE_MAX) {
        nb_map_entry *entry = nb_try_alloc(size);
        if (entry) {
            entry->alloc_size = size;
        }
        return entry;
    }

    if (!map->free_lists[index] && !map_refill_bin(map, index)) {
        return NULL;
    }
    void *slot = map->free_lists[index];
    map->free_lists[index] = *(void **)slot;
    nb_map_entry *entry = slot;
    entry->alloc_size = map_bin_size(index);
    return entry;
}

static void map_entry_free(nb_map *map, nb_map_entry *entry)
{
    if (!entry) {
        return;
    }
    size_t size = entry->alloc_size;
    size_t index = map_bin_index(size);
    if (index == SIZE_MAX) {
        nb_free(entry, size);
        return;
    }
    *(void **)entry = map->free_lists[index];
    map->free_lists[index] = entry;
}

static void map_free_slabs(nb_map *map)
{
    nb_map_slab *slab = map->slabs;
    while (slab) {
        nb_map_slab *next = slab->next;
        nb_free(slab, NB_MAP_SLAB_HDR + slab->count * slab->slot_size);
        slab = next;
    }
    map->slabs = NULL;
    for (size_t i = 0; i < NB_MAP_BIN_COUNT; i++) {
        map->free_lists[i] = NULL;
    }
}

static size_t next_power_of_two(size_t value)
{
    size_t result = 8u;
    while (result < value && result <= SIZE_MAX / 2u) {
        result *= 2u;
    }
    return result < value ? 0u : result;
}

static size_t bucket_index(const nb_map *map, uint64_t hash)
{
    return (size_t)hash & (map->nbuckets - 1u);
}

static nb_map_entry *map_find(const nb_map *map, nb_str key, uint64_t hash)
{
    if (!map || map->nbuckets == 0) {
        return NULL;
    }
    nb_map_entry *entry = map->buckets[bucket_index(map, hash)];
    while (entry) {
        if (entry->hash == hash && entry->klen == key.len &&
            (key.len == 0 || memcmp(entry->key, key.ptr, key.len) == 0)) {
            return entry;
        }
        entry = entry->bucket_next;
    }
    return NULL;
}

nb_map *nb_map_new(void)
{
    return nb_map_new_with_cap(8u);
}

nb_map *nb_map_new_with_cap(size_t cap)
{
    size_t nbuckets = next_power_of_two(cap);
    if (nbuckets == 0 || nbuckets > SIZE_MAX / sizeof(nb_map_entry *)) {
        return NULL;
    }
    nb_map *map = nb_try_alloc(sizeof(*map));
    if (!map) {
        return NULL;
    }
    nb_map_entry **buckets = nb_try_alloc(nbuckets * sizeof(*buckets));
    if (!buckets) {
        nb_free(map, sizeof(*map));
        return NULL;
    }
    memset(buckets, 0, nbuckets * sizeof(*buckets));
    map->buckets = buckets;
    map->nbuckets = nbuckets;
    map->size = 0;
    map->head = NULL;
    map->tail = NULL;
    map->dtor = NULL;
    map->dtor_ctx = NULL;
    map->seed = map_seed_new(map);
    memset(map->free_lists, 0, sizeof(map->free_lists));
    map->slabs = NULL;
    return map;
}

void nb_map_free(nb_map *map)
{
    if (!map) {
        return;
    }
    nb_map_clear(map);
    nb_free(map->buckets, map->nbuckets * sizeof(*map->buckets));
    nb_free(map, sizeof(*map));
}

static bool map_rehash(nb_map *map, size_t new_count)
{
    if (new_count < 8u || new_count > SIZE_MAX / sizeof(*map->buckets)) {
        return false;
    }
    nb_map_entry **buckets = nb_try_alloc(new_count * sizeof(*buckets));
    if (!buckets) {
        return false;
    }
    memset(buckets, 0, new_count * sizeof(*buckets));

    for (nb_map_entry *entry = map->head; entry; entry = entry->order_next) {
        size_t index = (size_t)entry->hash & (new_count - 1u);
        entry->bucket_next = buckets[index];
        buckets[index] = entry;
    }

    nb_free(map->buckets, map->nbuckets * sizeof(*map->buckets));
    map->buckets = buckets;
    map->nbuckets = new_count;
    return true;
}

static bool map_grow(nb_map *map)
{
    if (map->size < map->nbuckets - map->nbuckets / 4u) {
        return true;
    }
    if (map->nbuckets > SIZE_MAX / 2u) {
        return false;
    }
    return map_rehash(map, map->nbuckets * 2u);
}

void **nb_map_slot(nb_map *map, nb_str key, bool *created)
{
    if (created) {
        *created = false;
    }
    if (!map || (key.len != 0 && !key.ptr)) {
        return NULL;
    }
    size_t entry_size = map_entry_size(key.len);
    if (entry_size == 0) {
        return NULL;
    }

    uint64_t hash = map_hash(map, key);
    nb_map_entry *existing = map_find(map, key, hash);
    if (existing) {
        return &existing->value;
    }

    /* Allocate the entry first: a growth failure must leave the map's
     * observable state unchanged. */
    nb_map_entry *entry = map_entry_alloc(map, entry_size);
    if (!entry) {
        return NULL;
    }
    if (!map_grow(map)) {
        map_entry_free(map, entry);
        return NULL;
    }
    entry->hash = hash;
    entry->klen = key.len;
    entry->value = NULL;
    entry->bucket_next = NULL;
    entry->order_prev = map->tail;
    entry->order_next = NULL;
    if (key.len != 0) {
        memcpy(entry->key, key.ptr, key.len);
    }
    entry->key[key.len] = '\0';

    size_t index = bucket_index(map, hash);
    entry->bucket_next = map->buckets[index];
    map->buckets[index] = entry;

    if (map->tail) {
        map->tail->order_next = entry;
    } else {
        map->head = entry;
    }
    map->tail = entry;
    map->size++;
    if (created) {
        *created = true;
    }
    return &entry->value;
}

bool nb_map_put(nb_map *map, nb_str key, void *value, void **old_value)
{
    if (old_value) {
        *old_value = NULL;
    }
    bool created = false;
    void **slot = nb_map_slot(map, key, &created);
    if (!slot) {
        return false;
    }
    if (!created && old_value) {
        *old_value = *slot;
    }
    *slot = value;
    return true;
}

bool nb_map_put_cstr(nb_map *map, const char *key, void *value,
                     void **old_value)
{
    if (old_value) {
        *old_value = NULL;
    }
    return key ? nb_map_put(map, nb_str_from_cstr(key), value, old_value)
               : false;
}

void nb_map_set_value_dtor(nb_map *map, nb_map_value_dtor dtor, void *ctx)
{
    if (!map) {
        return;
    }
    map->dtor = dtor;
    map->dtor_ctx = ctx;
}

bool nb_map_reserve(nb_map *map, size_t min_capacity)
{
    if (!map) {
        return false;
    }
    if (min_capacity <= map->nbuckets - map->nbuckets / 4u) {
        return true;
    }
    if (min_capacity > SIZE_MAX / 4u) {
        return false;
    }
    /* map_grow() keeps the load factor at or below 3/4. Reserve enough
     * buckets for the requested number of entries, not just that many
     * buckets. */
    size_t target_entries = (min_capacity * 4u + 2u) / 3u;
    size_t target = next_power_of_two(target_entries);
    if (target == 0 || target <= map->nbuckets) {
        return false;
    }
    return map_rehash(map, target);
}

void *nb_map_get(const nb_map *map, nb_str key)
{
    if (!map || (key.len != 0 && !key.ptr)) {
        return NULL;
    }
    nb_map_entry *entry = map_find(map, key, map_hash(map, key));
    return entry ? entry->value : NULL;
}

void *nb_map_get_cstr(const nb_map *map, const char *key)
{
    return key ? nb_map_get(map, nb_str_from_cstr(key)) : NULL;
}

bool nb_map_has(const nb_map *map, nb_str key)
{
    if (!map || (key.len != 0 && !key.ptr)) {
        return false;
    }
    return map_find(map, key, map_hash(map, key)) != NULL;
}

bool nb_map_has_cstr(const nb_map *map, const char *key)
{
    return key ? nb_map_has(map, nb_str_from_cstr(key)) : false;
}

void *nb_map_del(nb_map *map, nb_str key)
{
    if (!map || (key.len != 0 && !key.ptr)) {
        return NULL;
    }
    uint64_t hash = map_hash(map, key);
    size_t index = bucket_index(map, hash);
    nb_map_entry *prev = NULL;
    nb_map_entry *entry = map->buckets[index];
    while (entry) {
        if (entry->hash == hash && entry->klen == key.len &&
            (key.len == 0 || memcmp(entry->key, key.ptr, key.len) == 0)) {
            break;
        }
        prev = entry;
        entry = entry->bucket_next;
    }
    if (!entry) {
        return NULL;
    }

    if (prev) {
        prev->bucket_next = entry->bucket_next;
    } else {
        map->buckets[index] = entry->bucket_next;
    }
    if (entry->order_prev) {
        entry->order_prev->order_next = entry->order_next;
    } else {
        map->head = entry->order_next;
    }
    if (entry->order_next) {
        entry->order_next->order_prev = entry->order_prev;
    } else {
        map->tail = entry->order_prev;
    }

    void *value = entry->value;
    map_entry_free(map, entry);
    map->size--;
    return value;
}

void *nb_map_del_cstr(nb_map *map, const char *key)
{
    return key ? nb_map_del(map, nb_str_from_cstr(key)) : NULL;
}

void nb_map_clear(nb_map *map)
{
    if (!map) {
        return;
    }
    nb_map_entry *entry = map->head;
    while (entry) {
        nb_map_entry *next = entry->order_next;
        if (map->dtor && entry->value) {
            map->dtor(entry->value, map->dtor_ctx);
        }
        if (map_bin_index(entry->alloc_size) == SIZE_MAX) {
            nb_free(entry, entry->alloc_size);
        }
        entry = next;
    }
    map_free_slabs(map);
    memset(map->buckets, 0, map->nbuckets * sizeof(*map->buckets));
    map->size = 0;
    map->head = NULL;
    map->tail = NULL;
}

size_t nb_map_size(const nb_map *map)
{
    return map ? map->size : 0u;
}

size_t nb_map_capacity(const nb_map *map)
{
    return map ? map->nbuckets : 0u;
}

void nb_map_iter_init(const nb_map *map, nb_map_iter *iter)
{
    NB_ASSERT(iter != NULL);
    iter->map = map;
    iter->entry = map ? map->head : NULL;
}

bool nb_map_iter_next(nb_map_iter *iter, nb_str *key, void **value)
{
    if (!iter || !iter->entry) {
        return false;
    }
    const nb_map_entry *entry = (const nb_map_entry *)iter->entry;
    if (key) {
        key->ptr = entry->key;
        key->len = entry->klen;
    }
    if (value) {
        *value = entry->value;
    }
    iter->entry = entry->order_next;
    return true;
}
