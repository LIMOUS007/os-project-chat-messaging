/*
 * test.h — a tiny unit-test harness (SPEC §11.1).
 *
 *   CHECK(cond)       fails if cond is false
 *   CHECK_EQ(a, b)    fails if a != b (integers), printing both values
 *   TEST_SUMMARY()    prints "N checks, M failed" and returns the exit code
 *
 * A test program is a main() that calls CHECK... and ends with
 * `return TEST_SUMMARY();` — non-zero if anything failed.
 */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>

static int test_checks = 0;
static int test_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        test_checks++;                                                           \
        if (!(cond)) {                                                           \
            test_failures++;                                                     \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        long long test_a_ = (long long)(a), test_b_ = (long long)(b);            \
        test_checks++;                                                           \
        if (test_a_ != test_b_) {                                                \
            test_failures++;                                                     \
            printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, \
                   #a, #b, test_a_, test_b_);                                    \
        }                                                                        \
    } while (0)

#define TEST_SUMMARY()                                                           \
    (printf("  %d checks, %d failed\n", test_checks, test_failures),             \
     test_failures == 0 ? 0 : 1)

#endif
