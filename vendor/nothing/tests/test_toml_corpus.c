/*
 * Runs the vendored toml-test corpus (TOML 1.1.0) through nb_toml_parse().
 *
 * Every valid/<...>.toml document must parse and every invalid/<...>.toml
 * document must be rejected. The corpus root is argv[1] when supplied, else
 * NB_TOML_TEST_DIR when defined at compile time, else "tests/toml-test"
 * relative to the working directory.
 */

#include "nb/buf.h"
#include "nb/err.h"
#include "nb/file.h"
#include "nb/str.h"
#include "nb/toml.h"

#include <stdio.h>
#include <string.h>

#define MAX_REPORTED_FAILURES 20
#define DETAIL_CAP 320

typedef struct corpus_result {
    size_t valid_total;
    size_t valid_passed;
    size_t invalid_total;
    size_t invalid_passed;
    size_t failures;
    size_t reported;
} corpus_result;

static const char *default_corpus_root(void)
{
#ifdef NB_TOML_TEST_DIR
    return NB_TOML_TEST_DIR;
#else
    return "tests/toml-test";
#endif
}

static void report_failure(corpus_result *result, const char *relative,
                           const char *detail)
{
    result->failures++;
    if (result->reported >= MAX_REPORTED_FAILURES) {
        return;
    }
    fprintf(stderr, "FAIL %s: %s\n", relative, detail);
    result->reported++;
}

static void run_case(const char *root, const char *relative,
                     bool expect_valid, corpus_result *result)
{
    if (expect_valid) {
        result->valid_total++;
    } else {
        result->invalid_total++;
    }

    nb_buf full;
    nb_buf input;
    nb_buf_init(&full);
    nb_buf_init(&input);

    if (nb_path_join(&full, root, relative) != NB_OK) {
        report_failure(result, relative, "cannot build corpus path");
        nb_buf_destroy(&input);
        nb_buf_destroy(&full);
        return;
    }

    nb_err read_err;
    nb_err_clear(&read_err);
    if (nb_file_read_all(nb_buf_cstr(&full), &input, 0, &read_err) != NB_OK) {
        char detail[DETAIL_CAP];
        snprintf(detail, sizeof(detail), "read failed: code=%d msg=%s",
                 (int)read_err.code, read_err.msg);
        report_failure(result, relative, detail);
        nb_buf_destroy(&input);
        nb_buf_destroy(&full);
        return;
    }

    nb_err parse_err;
    nb_err_clear(&parse_err);
    nb_toml *toml = nb_toml_parse(nb_buf_str(&input), &parse_err);

    if (expect_valid) {
        if (toml != NULL) {
            result->valid_passed++;
            nb_toml_free(toml);
        } else {
            char detail[DETAIL_CAP];
            snprintf(detail, sizeof(detail), "expected valid TOML, code=%d msg=%s",
                     (int)parse_err.code, parse_err.msg);
            report_failure(result, relative, detail);
        }
    } else if (toml == NULL) {
        result->invalid_passed++;
    } else {
        nb_toml_free(toml);
        report_failure(result, relative,
                       "expected invalid TOML, but parsing succeeded");
    }

    nb_buf_destroy(&input);
    nb_buf_destroy(&full);
}

static void walk(const char *root, const char *relative, bool expect_valid,
                 corpus_result *result)
{
    nb_buf dir_path;
    nb_buf_init(&dir_path);

    if (nb_path_join(&dir_path, root, relative) != NB_OK) {
        report_failure(result, relative, "cannot build directory path");
        nb_buf_destroy(&dir_path);
        return;
    }

    nb_err err;
    nb_err_clear(&err);
    nb_dir *dir = nb_dir_open(nb_buf_cstr(&dir_path), &err);
    if (dir == NULL) {
        char detail[DETAIL_CAP];
        snprintf(detail, sizeof(detail), "cannot open directory: code=%d msg=%s",
                 (int)err.code, err.msg);
        report_failure(result, relative, detail);
        nb_buf_destroy(&dir_path);
        return;
    }

    const char *name;
    while ((name = nb_dir_next(dir, NULL)) != NULL) {
        nb_buf child_relative;
        nb_buf child_path;
        nb_buf_init(&child_relative);
        nb_buf_init(&child_path);

        if (nb_path_join(&child_relative, relative, name) != NB_OK ||
            nb_path_join(&child_path, nb_buf_cstr(&dir_path), name) != NB_OK) {
            report_failure(result, relative, "cannot build child path");
            nb_buf_destroy(&child_path);
            nb_buf_destroy(&child_relative);
            continue;
        }

        if (nb_file_is_dir(nb_buf_cstr(&child_path))) {
            walk(root, nb_buf_cstr(&child_relative), expect_valid, result);
        } else {
            const char *extension = nb_path_extension(nb_buf_cstr(&child_path));
            if (extension != NULL && strcmp(extension, "toml") == 0) {
                run_case(root, nb_buf_cstr(&child_relative), expect_valid,
                         result);
            }
        }

        nb_buf_destroy(&child_path);
        nb_buf_destroy(&child_relative);
    }

    if (nb_dir_status(dir) != NB_OK) {
        report_failure(result, relative, "directory iteration failed");
    }
    nb_dir_close(dir);
    nb_buf_destroy(&dir_path);
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "usage: %s [corpus-root]\n", argv[0]);
        return 1;
    }

    const char *root = argc > 1 ? argv[1] : default_corpus_root();
    corpus_result result = {0};

    walk(root, "valid", true, &result);
    walk(root, "invalid", false, &result);

    printf("valid: %zu/%zu, invalid: %zu/%zu\n", result.valid_passed,
           result.valid_total, result.invalid_passed, result.invalid_total);

    if (result.failures > result.reported) {
        fprintf(stderr, "... %zu more failures not shown\n",
                result.failures - result.reported);
    }

    if (result.valid_total == 0 || result.invalid_total == 0) {
        fprintf(stderr, "corpus root '%s' is missing valid or invalid cases\n",
                root);
        return 1;
    }

    return result.failures == 0 ? 0 : 1;
}
