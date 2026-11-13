#include "nb/nbase.h"

#include <stdio.h>
#include <string.h>

int main(void)
{
    const char *text =
        "name = nbase\n"
        "[server]\n"
        "host = 127.0.0.1\n"
        "port = 8080\n"
        "url = http://x/#frag\n"
        "note = plain ; ignored comment\n"
        "quoted = \"hello # world\"\n"
        "escaped = \"line1\\nline2\"\n";
    nb_err err;
    nb_err_clear(&err);
    nb_ini *ini = nb_ini_from_string(nb_str_from_cstr(text), &err);
    if (!ini) {
        printf("parse failed: %s\n", err.msg);
        return 1;
    }

    printf("name=%s host=%s port=%lld\n",
           nb_ini_get(ini, "", "name"),
           nb_ini_get(ini, "server", "host"),
           (long long)nb_ini_get_int(ini, "server", "port", 0));
    printf("url=%s\n", nb_ini_get(ini, "server", "url"));
    printf("note=%s\n", nb_ini_get(ini, "server", "note"));
    printf("quoted=%s\n", nb_ini_get(ini, "server", "quoted"));
    printf("escaped_len=%zu\n",
           strlen(nb_ini_get(ini, "server", "escaped")));

    nb_ini_set(ini, "server", "port", "9090");
    nb_ini_set(ini, "server", "empty", "");
    nb_ini_set(ini, "server", "padded", "  padded  ");
    nb_ini_set(ini, "server", "hash", "a#b");
    nb_ini_set(ini, "server", "path", "C:\\dir");

    nb_buf *out = nb_buf_new();
    if (out && nb_ini_write(ini, out) == NB_OK) {
        printf("--- written INI ---\n%s", nb_buf_cstr(out));
    }
    nb_buf_free(out);

    nb_buf *path = nb_buf_new();
    if (!path || nb_file_temp(path, &err) != NB_OK) {
        printf("temp failed\n");
        nb_buf_free(path);
        nb_ini_free(ini);
        return 1;
    }
    if (nb_ini_save(ini, nb_buf_cstr(path), &err) != NB_OK) {
        printf("save failed\n");
        nb_buf_free(path);
        nb_ini_free(ini);
        return 1;
    }
    nb_ini *loaded = nb_ini_load(nb_buf_cstr(path), &err);
    if (!loaded) {
        printf("load failed\n");
        nb_file_remove(nb_buf_cstr(path), &err);
        nb_buf_free(path);
        nb_ini_free(ini);
        return 1;
    }
    printf("saved hash=%s empty='%s' path=%s\n",
           nb_ini_get(loaded, "server", "hash"),
           nb_ini_get(loaded, "server", "empty"),
           nb_ini_get(loaded, "server", "path"));
    nb_ini_free(loaded);
    nb_file_remove(nb_buf_cstr(path), &err);
    nb_buf_free(path);
    nb_ini_free(ini);
    return 0;
}
