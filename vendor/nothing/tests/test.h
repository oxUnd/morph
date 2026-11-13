#ifndef NB_TEST_H
#define NB_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int nb_test_failures;

#define NB_CHECK(cond)                                                      \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            nb_test_failures++;                                             \
        }                                                                   \
    } while (0)

#define NB_CHECK_STR(actual, expected)                                      \
    do {                                                                    \
        const char *nb_a = (actual);                                        \
        const char *nb_e = (expected);                                      \
        if (!nb_a || !nb_e || strcmp(nb_a, nb_e) != 0) {                    \
            fprintf(stderr, "FAIL %s:%d: '%s' != '%s'\n", __FILE__,         \
                    __LINE__, nb_a ? nb_a : "(null)",                       \
                    nb_e ? nb_e : "(null)");                                \
            nb_test_failures++;                                             \
        }                                                                   \
    } while (0)

#define NB_CHECK_INT(actual, expected)                                      \
    do {                                                                    \
        long long nb_a = (long long)(actual);                               \
        long long nb_e = (long long)(expected);                             \
        if (nb_a != nb_e) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %lld != %lld\n", __FILE__,         \
                    __LINE__, nb_a, nb_e);                                  \
            nb_test_failures++;                                             \
        }                                                                   \
    } while (0)

#define NB_CHECK_MEM(actual, expected, len)                                 \
    do {                                                                    \
        if (memcmp((actual), (expected), (len)) != 0) {                     \
            fprintf(stderr, "FAIL %s:%d: memory mismatch\n", __FILE__,      \
                    __LINE__);                                              \
            nb_test_failures++;                                             \
        }                                                                   \
    } while (0)

void test_core(void);
void test_system(void);
void test_containers(void);
void test_text(void);
void test_config(void);
void test_toml(void);
void test_file_time(void);
void test_runtime(void);
void test_utf8(void);
void test_hash(void);
void test_regressions(void);
void test_process(void);
void test_fs_extra(void);
void test_core_extra(void);
void test_signal(void);
void test_log_file(void);

#endif /* NB_TEST_H */
