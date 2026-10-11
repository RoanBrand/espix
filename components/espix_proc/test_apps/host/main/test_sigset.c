#include "unity.h"

#include "espix_sigset.h"

static void test_sigbit_range(void)
{
    TEST_ASSERT_EQUAL_HEX32(0, espix_sigbit(0));
    TEST_ASSERT_EQUAL_HEX32(0, espix_sigbit(-1));
    TEST_ASSERT_EQUAL_HEX32(0, espix_sigbit(NSIG));
    TEST_ASSERT_EQUAL_HEX32(1u << 1, espix_sigbit(SIGHUP));
    /* SIGUSR2 is the top bit, where a signed `1 << sig` goes wrong. */
    TEST_ASSERT_EQUAL_HEX32(0x80000000u, espix_sigbit(31));
}

static void test_stopping_bit_is_no_signal(void)
{
    for (int sig = 1; sig < NSIG; sig++) {
        TEST_ASSERT_EQUAL_HEX32(0, espix_sigbit(sig) & ESPIX_SIG_STOPPING);
    }
}

static void test_uncatchable(void)
{
    TEST_ASSERT_NOT_EQUAL(0, ESPIX_SIG_UNCATCHABLE & espix_sigbit(SIGKILL));
    TEST_ASSERT_NOT_EQUAL(0, ESPIX_SIG_UNCATCHABLE & espix_sigbit(SIGSTOP));
    TEST_ASSERT_EQUAL_HEX32(0, ESPIX_SIG_UNCATCHABLE & espix_sigbit(SIGTERM));
}

static void test_ticks_saturate_below_forever(void)
{
    TEST_ASSERT_EQUAL_UINT32(250, espix_ticks_from_us(1000000, 250, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(0, espix_ticks_from_us(3999, 250, UINT32_MAX));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX - 1,
                             espix_ticks_from_us(UINT64_MAX / 1000, 250,
                                                 UINT32_MAX));
}

static void test_interrupted_sleep_never_reports_zero(void)
{
    TEST_ASSERT_EQUAL_UINT(1, espix_sleep_secs_left(1));
    TEST_ASSERT_EQUAL_UINT(1, espix_sleep_secs_left(1000));
    TEST_ASSERT_EQUAL_UINT(2, espix_sleep_secs_left(1001));
    TEST_ASSERT_EQUAL_UINT(0, espix_sleep_secs_left(0));
}

void run_sigset_tests(void)
{
    RUN_TEST(test_sigbit_range);
    RUN_TEST(test_stopping_bit_is_no_signal);
    RUN_TEST(test_uncatchable);
    RUN_TEST(test_ticks_saturate_below_forever);
    RUN_TEST(test_interrupted_sleep_never_reports_zero);
}
