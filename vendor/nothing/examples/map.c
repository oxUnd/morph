#include "nb/nbase.h"

#include <stdio.h>

int main(void)
{
    nb_map *scores = nb_map_new();
    if (!scores) {
        return 1;
    }
    nb_map_put_cstr(scores, "alice", (void *)(intptr_t)95, NULL);
    nb_map_put_cstr(scores, "bob", (void *)(intptr_t)88, NULL);
    nb_map_put_cstr(scores, "carol", (void *)(intptr_t)91, NULL);

    printf("bob=%lld size=%zu\n",
           (long long)(intptr_t)nb_map_get_cstr(scores, "bob"),
           nb_map_size(scores));

    void *old = NULL;
    if (nb_map_put_cstr(scores, "bob", (void *)(intptr_t)99, &old)) {
        printf("bob updated from %lld to %lld\n",
               (long long)(intptr_t)old,
               (long long)(intptr_t)nb_map_get_cstr(scores, "bob"));
    }
    printf("reserve=%d capacity=%zu\n",
           nb_map_reserve(scores, 64) ? 1 : 0,
           nb_map_capacity(scores));

    nb_map_iter it;
    nb_map_iter_init(scores, &it);
    nb_str key;
    void *value;
    while (nb_map_iter_next(&it, &key, &value)) {
        printf("  %.*s = %lld\n", (int)key.len, key.ptr,
               (long long)(intptr_t)value);
    }

    nb_map_del_cstr(scores, "bob");
    printf("after delete: size=%zu\n", nb_map_size(scores));
    nb_map_free(scores);
    return 0;
}
