/*
 * test_jobqueue.c — U4: the bounded producer-consumer queue.
 *   - 4 producers + 4 consumers move 100,000 items: none lost or duplicated
 *   - try_push on a full queue returns false without blocking
 *   - close wakes blocked consumers, which then return false
 */
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#include "jobqueue.h"
#include "test.h"

#define ITEMS      100000
#define PRODUCERS  4
#define CONSUMERS  4

static jobqueue_t q;
static int        received[ITEMS];          /* how often each item was popped */
static pthread_mutex_t received_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *producer(void *arg)
{
    int first = *(int *)arg;
    for (int i = first; i < ITEMS; i += PRODUCERS) {
        job_t job;
        memset(&job, 0, sizeof job);
        job.h.slot = (uint32_t)i;            /* the item number travels in the job */
        while (!jobq_try_push(&q, &job))
            sched_yield();                   /* full: try again (never blocks) */
    }
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    job_t job;
    while (jobq_pop(&q, &job)) {
        pthread_mutex_lock(&received_mutex);
        received[job.h.slot]++;
        pthread_mutex_unlock(&received_mutex);
    }
    return NULL;
}

static void test_many_threads(void)
{
    jobq_init(&q);
    pthread_t prod[PRODUCERS], cons[CONSUMERS];
    int starts[PRODUCERS];
    for (int i = 0; i < CONSUMERS; i++)
        pthread_create(&cons[i], NULL, consumer, NULL);
    for (int i = 0; i < PRODUCERS; i++) {
        starts[i] = i;
        pthread_create(&prod[i], NULL, producer, &starts[i]);
    }
    for (int i = 0; i < PRODUCERS; i++)
        pthread_join(prod[i], NULL);
    jobq_close(&q);                          /* consumers drain, then exit */
    for (int i = 0; i < CONSUMERS; i++)
        pthread_join(cons[i], NULL);

    int wrong = 0;
    for (int i = 0; i < ITEMS; i++)
        if (received[i] != 1)
            wrong++;
    CHECK_EQ(wrong, 0);
    jobq_destroy(&q);
}

static void test_full_queue(void)
{
    jobq_init(&q);
    job_t job;
    memset(&job, 0, sizeof job);
    int pushed = 0;
    while (pushed < JOBQ_CAPACITY + 10 && jobq_try_push(&q, &job))
        pushed++;
    CHECK_EQ(pushed, JOBQ_CAPACITY);
    CHECK(!jobq_try_push(&q, &job));        /* returns false, does not block */
    jobq_close(&q);
    CHECK(!jobq_try_push(&q, &job));        /* closed: no more pushes */
    int popped = 0;
    while (jobq_pop(&q, &job))              /* but queued jobs are still handed out */
        popped++;
    CHECK_EQ(popped, JOBQ_CAPACITY);
    jobq_destroy(&q);
}

static void *blocked_consumer(void *arg)
{
    job_t job;
    *(int *)arg = jobq_pop(&q, &job) ? 1 : 0;
    return NULL;
}

static void test_close_wakes_consumers(void)
{
    jobq_init(&q);
    pthread_t t[3];
    int results[3] = { -1, -1, -1 };
    for (int i = 0; i < 3; i++)
        pthread_create(&t[i], NULL, blocked_consumer, &results[i]);
    struct timespec pause = { 0, 50 * 1000 * 1000 };
    nanosleep(&pause, NULL);                /* let them block in jobq_pop */
    jobq_close(&q);
    for (int i = 0; i < 3; i++) {
        pthread_join(t[i], NULL);
        CHECK_EQ(results[i], 0);
    }
    jobq_destroy(&q);
}

int main(void)
{
    test_many_threads();
    test_full_queue();
    test_close_wakes_consumers();
    return TEST_SUMMARY();
}
