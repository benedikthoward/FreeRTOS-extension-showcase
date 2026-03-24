#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"

void test_harness_init(void);
void test_harness_print(const char *tag, const char *msg);

#endif /* TEST_HARNESS_H */
