#include <stdlib.h>

#include "unity.h"

void run_sigset_tests(void);
void run_threads_tests(void);

void setUp(void) {}
void tearDown(void) {}

void app_main(void)
{
    UNITY_BEGIN();
    run_sigset_tests();
    run_threads_tests();
    exit(UNITY_END());
}
