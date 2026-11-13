#include "nb/nbase.h"

#include <stdio.h>

struct point {
    int x;
    int y;
};

int main(void)
{
    nb_arena *arena = nb_arena_new(4096);
    if (!arena) {
        return 1;
    }
    int *values = nb_arena_alloc(arena, 10u * sizeof(*values));
    if (!values) {
        nb_arena_free(arena);
        return 1;
    }
    int sum = 0;
    for (int i = 0; i < 10; i++) {
        values[i] = i * i;
        sum += values[i];
    }
    printf("arena: used=%zu total=%zu sum=%d\n",
           nb_arena_used(arena), nb_arena_total(arena), sum);
    nb_arena_reset(arena);
    printf("arena after reset: used=%zu\n", nb_arena_used(arena));
    nb_arena_free(arena);

    nb_pool *pool = nb_pool_new(sizeof(struct point), 8);
    if (!pool) {
        return 1;
    }
    struct point *p = nb_pool_alloc(pool);
    if (!p) {
        nb_pool_free(pool);
        return 1;
    }
    p->x = 3;
    p->y = 4;
    printf("pool: point=(%d,%d) used=%zu capacity=%zu\n",
           p->x, p->y, nb_pool_used(pool), nb_pool_capacity(pool));
    nb_pool_release(pool, p);
    nb_pool_free(pool);
    return 0;
}
