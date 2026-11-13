#include "nb/nbase.h"

#include <stdio.h>

int main(void)
{
    nb_str line = NB_STR_LIT("  port = 8080  ");
    nb_str trimmed = nb_str_trim(line);
    size_t equals = nb_str_find(trimmed, NB_STR_LIT("="));
    if (equals == SIZE_MAX) {
        return 1;
    }
    nb_str key = nb_str_rtrim(nb_str_from_parts(trimmed.ptr, equals));
    nb_str value = nb_str_trim(
        nb_str_from_parts(trimmed.ptr + equals + 1u,
                          trimmed.len - equals - 1u));
    int64_t port = 0;
    bool parsed = nb_str_to_i64(value, &port);
    printf("key=%.*s value=%.*s parsed=%d port=%lld\n",
           (int)key.len, key.ptr, (int)value.len, value.ptr,
           parsed ? 1 : 0, (long long)port);
    printf("hash=0x%016llx\n",
           (unsigned long long)nb_str_hash(key));

    printf("icase=%d find_char=%zu\n",
           nb_str_eq_icase(NB_STR_LIT("Port"), NB_STR_LIT("port")) ? 1 : 0,
           nb_str_find_char(trimmed, '='));

    nb_split_iter split;
    nb_split_init(&split, NB_STR_LIT("alpha,beta,,gamma"), ',');
    nb_str token;
    printf("split:");
    while (nb_split_next(&split, &token)) {
        printf(" [%.*s]", token.ptr ? (int)token.len : 0, token.ptr);
    }
    printf("\n");
    return parsed ? 0 : 1;
}
