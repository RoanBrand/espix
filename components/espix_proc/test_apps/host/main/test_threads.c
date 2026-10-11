#include "unity.h"

#include "proc_threads.h"

#define CAP 4

static espix_thread_t  v[CAP];
static espix_threads_t t = { v, CAP };

static int a, b;    /* stand-ins for two process slots */
static int ta1, ta2, tb1;

static void reset(void)
{
    for (int i = 0; i < CAP; i++) {
        v[i] = (espix_thread_t){ 0 };
    }
}

static void test_full_table_refuses(void)
{
    reset();
    for (int i = 0; i < CAP; i++) {
        TEST_ASSERT_TRUE(espix_threads_add(&t, &a, 10 + i));
    }
    TEST_ASSERT_FALSE(espix_threads_add(&t, &a, 99));
    TEST_ASSERT_TRUE(espix_threads_remove(&t, 11, NULL));
    TEST_ASSERT_TRUE(espix_threads_add(&t, &b, 99));
}

static void test_refuses_no_slot_and_duplicate_tid(void)
{
    reset();
    TEST_ASSERT_FALSE(espix_threads_add(&t, NULL, 1));
    TEST_ASSERT_TRUE(espix_threads_add(&t, &a, 1));
    TEST_ASSERT_FALSE(espix_threads_add(&t, &b, 1));
}

static void test_task_is_set_by_the_thread(void)
{
    reset();
    TEST_ASSERT_TRUE(espix_threads_add(&t, &a, 7));
    TEST_ASSERT_EQUAL_INT32(-1, espix_threads_tid_of(&t, &ta1));
    TEST_ASSERT_TRUE(espix_threads_set_task(&t, 7, &ta1, NULL));
    TEST_ASSERT_EQUAL_INT32(7, espix_threads_tid_of(&t, &ta1));
    TEST_ASSERT_EQUAL_INT32(-1, espix_threads_tid_of(&t, NULL));
}

static void test_set_task_after_reap_says_gone(void)
{
    reset();
    TEST_ASSERT_TRUE(espix_threads_add(&t, &a, 7));
    TEST_ASSERT_TRUE(espix_threads_take(&t, &a, NULL));
    TEST_ASSERT_FALSE(espix_threads_set_task(&t, 7, &ta1, NULL));
}

static void test_of_and_take_are_per_process(void)
{
    reset();
    espix_threads_add(&t, &a, 1);
    espix_threads_add(&t, &b, 2);
    espix_threads_add(&t, &a, 3);
    espix_threads_set_task(&t, 1, &ta1, NULL);
    espix_threads_set_task(&t, 2, &tb1, NULL);
    espix_threads_set_task(&t, 3, &ta2, NULL);

    espix_thread_t out[CAP];
    TEST_ASSERT_EQUAL_size_t(2, espix_threads_of(&t, &a, out, CAP));
    TEST_ASSERT_EQUAL_INT32(1, out[0].tid);
    TEST_ASSERT_EQUAL_PTR(&ta2, out[1].task);
    /* The count is the slot's, even when the copy is cut short. */
    TEST_ASSERT_EQUAL_size_t(2, espix_threads_of(&t, &a, out, 1));

    espix_thread_t got;
    TEST_ASSERT_TRUE(espix_threads_take(&t, &a, &got));
    TEST_ASSERT_TRUE(espix_threads_take(&t, &a, &got));
    TEST_ASSERT_FALSE(espix_threads_take(&t, &a, &got));
    TEST_ASSERT_EQUAL_size_t(1, espix_threads_of(&t, &b, out, CAP));
    TEST_ASSERT_EQUAL_INT32(2, out[0].tid);
}

static void test_remove_copies_the_entry(void)
{
    reset();
    espix_threads_add(&t, &a, 5);
    espix_threads_set_task(&t, 5, &ta1, &tb1);

    espix_thread_t got;
    TEST_ASSERT_TRUE(espix_threads_remove(&t, 5, &got));
    TEST_ASSERT_EQUAL_PTR(&a, got.slot);
    TEST_ASSERT_EQUAL_PTR(&ta1, got.task);
    TEST_ASSERT_EQUAL_PTR(&tb1, got.reent);
    TEST_ASSERT_FALSE(espix_threads_remove(&t, 5, NULL));
    TEST_ASSERT_EQUAL_size_t(0, espix_threads_of(&t, &a, NULL, 0));
}

void run_threads_tests(void)
{
    RUN_TEST(test_full_table_refuses);
    RUN_TEST(test_refuses_no_slot_and_duplicate_tid);
    RUN_TEST(test_task_is_set_by_the_thread);
    RUN_TEST(test_set_task_after_reap_says_gone);
    RUN_TEST(test_of_and_take_are_per_process);
    RUN_TEST(test_remove_copies_the_entry);
}
