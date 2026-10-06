/* harness.h — minimal dependency-free test harness (hosted builds only). */
#ifndef TINYACTORS_TESTS_HARNESS_HARNESS_H
#define TINYACTORS_TESTS_HARNESS_HARNESS_H

#include <stdio.h>

static int ta_test_failures;
static int ta_test_checks;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ta_test_checks++;                                                                          \
        if (!(cond)) {                                                                             \
            ta_test_failures++;                                                                    \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                                          \
    } while (0)

#define CHECK_EQUAL(a, b)                                                                          \
    do {                                                                                           \
        unsigned long long ta__a = (unsigned long long)(a), ta__b = (unsigned long long)(b);       \
        ta_test_checks++;                                                                          \
        if (ta__a != ta__b) {                                                                      \
            ta_test_failures++;                                                                    \
            fprintf(stderr, "%s:%d: CHECK_EQUAL failed: %s == %s (%llu vs %llu)\n", __FILE__,      \
                    __LINE__, #a, #b, ta__a, ta__b);                                               \
        }                                                                                          \
    } while (0)

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        int ta__before = ta_test_failures;                                                         \
        fn();                                                                                      \
        fprintf(stderr, "%s %s\n", ta_test_failures == ta__before ? "ok  " : "FAIL", #fn);         \
    } while (0)

#define TEST_MAIN_END()                                                                            \
    do {                                                                                           \
        fprintf(stderr, "%d checks, %d failures\n", ta_test_checks, ta_test_failures);             \
        return ta_test_failures ? 1 : 0;                                                           \
    } while (0)

#endif /* TINYACTORS_TESTS_HARNESS_HARNESS_H */
