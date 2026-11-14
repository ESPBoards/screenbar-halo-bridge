/* SPDX-License-Identifier: GPL-3.0-only */
/* Every test's CHECK: count the failure and carry on, so one run shows them all. */
#pragma once
#include <stdio.h>

static unsigned failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

/* The exit status for main. */
static inline int check_report(const char *name)
{
    printf("%s: %s\n", name, failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
