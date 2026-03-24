#include "test_harness.h"
#include <stdio.h>

void test_harness_init(void)
{
    printf("[test_harness] initialized\n");
}

void test_harness_print(const char *tag, const char *msg)
{
    printf("[%s] t=%lu: %s\n", tag, (unsigned long)xTaskGetTickCount(), msg);
}
