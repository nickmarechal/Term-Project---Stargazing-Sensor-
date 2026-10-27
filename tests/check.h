#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

/* The totals live in test_main.c. Every suite adds to the same two counters,
 * so main can print one summary and exit non-zero if anything failed. */
extern int check_count;
extern int check_failures;

#define CHECK(cond, ...)                                                \
    do {                                                                \
        check_count++;                                                  \
        if (!(cond)) {                                                  \
            check_failures++;                                           \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                               \
            fprintf(stderr, "\n");                                      \
        }                                                               \
    } while (0)

#endif
