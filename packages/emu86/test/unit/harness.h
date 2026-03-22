#ifndef EMU86_TEST_HARNESS_H
#define EMU86_TEST_HARNESS_H

#include <stdio.h>

#define TEST(name) static void test_##name(void)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        test_failures++; \
    } else { \
        test_passes++; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        printf("FAIL: %s:%d: %s != %s (%lld != %lld)\n", \
               __FILE__, __LINE__, #a, #b, _a, _b); \
        test_failures++; \
    } else { \
        test_passes++; \
    } \
} while(0)

#define RUN_TEST(name) do { \
    printf("  %s...", #name); \
    test_##name(); \
    printf(" ok\n"); \
} while(0)

int test_passes = 0;
int test_failures = 0;

#endif /* EMU86_TEST_HARNESS_H */
