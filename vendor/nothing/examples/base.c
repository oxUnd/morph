#include "nb/nbase.h"

#include <stdio.h>

int main(void)
{
    int values[] = { 4, 8, 15, 16, 23, 42 };
    printf("nbase %s\n", nb_version());
    printf("array size: %zu\n", NB_ARRAY_SIZE(values));
    printf("min=%d max=%d clamp=%d\n",
           NB_MIN(3, 9), NB_MAX(3, 9), NB_CLAMP(12, 0, 10));
    NB_ASSERT(NB_ARRAY_SIZE(values) == 6);
    return 0;
}
