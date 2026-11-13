#include "test.h"

#include <stdbool.h>

int nb_test_failures = 0;

static bool run_named(const char *name)
{
    if (strcmp(name, "core") == 0) {
        test_core();
    } else if (strcmp(name, "system") == 0) {
        test_system();
    } else if (strcmp(name, "containers") == 0) {
        test_containers();
    } else if (strcmp(name, "text") == 0) {
        test_text();
    } else if (strcmp(name, "config") == 0) {
        test_config();
    } else if (strcmp(name, "toml") == 0) {
        test_toml();
    } else if (strcmp(name, "file_time") == 0) {
        test_file_time();
    } else if (strcmp(name, "runtime") == 0) {
        test_runtime();
    } else if (strcmp(name, "utf8") == 0) {
        test_utf8();
    } else if (strcmp(name, "regressions") == 0) {
        test_regressions();
    } else if (strcmp(name, "hash") == 0) {
        test_hash();
    } else if (strcmp(name, "process") == 0) {
        test_process();
    } else if (strcmp(name, "fs_extra") == 0) {
        test_fs_extra();
    } else if (strcmp(name, "core_extra") == 0) {
        test_core_extra();
        test_signal();
    } else if (strcmp(name, "log_file") == 0) {
        test_log_file();
    } else {
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (!run_named(argv[1])) {
            fprintf(stderr, "unknown test suite: %s\n", argv[1]);
            return 2;
        }
        if (nb_test_failures != 0) {
            fprintf(stderr, "%d test(s) failed\n", nb_test_failures);
            return 1;
        }
        printf("all tests passed\n");
        return 0;
    }

    test_core();
    test_system();
    test_containers();
    test_text();
    test_config();
    test_toml();
    test_file_time();
    test_runtime();
    test_utf8();
    test_hash();
    test_regressions();
    test_process();
    test_fs_extra();
    test_core_extra();
    test_signal();
    test_log_file();
    if (nb_test_failures != 0) {
        fprintf(stderr, "%d test(s) failed\n", nb_test_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
