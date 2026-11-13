#include "nb/nbase.h"

#include <stdio.h>

static void print_vec(const char *label, const nb_vec *vec)
{
    printf("%s: len=%zu values=", label, nb_vec_len(vec));
    for (size_t i = 0; i < nb_vec_len(vec); i++) {
        printf("%s%d", i == 0 ? "" : " ", *(const int *)nb_vec_at(vec, i));
    }
    printf("\n");
}

int main(void)
{
    nb_vec numbers;
    nb_vec_init(&numbers, sizeof(int));
    for (int i = 0; i < 5; i++) {
        int *slot = nb_vec_push(&numbers);
        if (!slot) {
            nb_vec_destroy(&numbers);
            return 1;
        }
        *slot = i * 10;
    }

    int inserted = 25;
    if (!nb_vec_insert(&numbers, 2, &inserted)) {
        nb_vec_destroy(&numbers);
        return 1;
    }
    print_vec("after insert", &numbers);

    nb_vec_remove(&numbers, 0);
    print_vec("after remove", &numbers);

    nb_vec_destroy(&numbers);
    return 0;
}
