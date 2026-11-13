#include "nb/nbase.h"

#include <stdio.h>

int main(void)
{
    nb_map *config = nb_map_new();
    nb_map_put_cstr(config, "name", (void *)"nbase", NULL);
    nb_map_put_cstr(config, "version", (void *)nb_version(), NULL);

    nb_buf *line = nb_buf_new();
    nb_buf_printf(line, "%s %s", (const char *)nb_map_get_cstr(config, "name"),
                  (const char *)nb_map_get_cstr(config, "version"));
    printf("%s\n", nb_buf_cstr(line));

    nb_log_write(nb_log_default(), NB_LOG_INFO, "example started");
    printf("encoded: ");
    nb_buf *encoded = nb_buf_new();
    if (nb_base64_encode("hello", 5, NB_B64_STD, true, encoded, NULL) == NB_OK) {
        printf("%s\n", nb_buf_cstr(encoded));
    }

    nb_buf_free(encoded);
    nb_buf_free(line);
    nb_map_free(config);
    return 0;
}
