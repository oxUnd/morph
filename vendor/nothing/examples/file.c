#include "nb/nbase.h"

#include <stdio.h>

int main(void)
{
    nb_err err;
    nb_err_clear(&err);
    nb_buf *dir = nb_buf_new();
    nb_buf *path = nb_buf_new();
    nb_buf *data = nb_buf_new();
    nb_buf *clean = nb_buf_new();
    static const char payload[] = "atomic write\n";
    bool temp_exists = false;
    bool dir_exists = false;
    int rc = 1;

    if (!dir || !path || !data || !clean) {
        nb_buf_free(clean);
        nb_buf_free(data);
        nb_buf_free(path);
        nb_buf_free(dir);
        return 1;
    }

    nb_status status = nb_file_temp(dir, &err);
    if (status == NB_OK) {
        temp_exists = true;
        status = nb_file_remove(nb_buf_cstr(dir), &err);
    }
    if (status == NB_OK) {
        temp_exists = false;
        status = nb_file_mkdir(nb_buf_cstr(dir), false, &err);
    }
    if (status == NB_OK) {
        dir_exists = true;
        status = nb_path_join(path, nb_buf_cstr(dir), "note.txt");
    }
    if (status == NB_OK) {
        status = nb_file_write_all_atomic(nb_buf_cstr(path), payload,
                                          sizeof(payload) - 1u, NULL, &err);
    }
    if (status == NB_OK) {
        status = nb_file_read_all(nb_buf_cstr(path), data, 0, &err);
    }
    if (status == NB_OK) {
        status = nb_path_clean(clean, "/tmp//demo/./nested/../note.txt");
    }
    if (status == NB_OK) {
        printf("content=%s", nb_buf_cstr(data));
        printf("clean=%s\n", nb_buf_cstr(clean));
        rc = 0;
    }

    if (dir_exists) {
        (void)nb_file_remove(nb_buf_cstr(path), &err);
        (void)nb_file_rmdir(nb_buf_cstr(dir), &err);
    } else if (temp_exists) {
        (void)nb_file_remove(nb_buf_cstr(dir), &err);
    }
    nb_buf_free(clean);
    nb_buf_free(path);
    nb_buf_free(data);
    nb_buf_free(dir);
    return rc;
}
