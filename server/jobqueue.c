/*
 * jobqueue.c — the classic bounded buffer with one mutex and one condvar.
 */
#include "jobqueue.h"

void jobq_init(jobqueue_t *q)
{
    q->head = 0;
    q->count = 0;
    q->closed = false;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

void jobq_destroy(jobqueue_t *q)
{
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
}

bool jobq_try_push(jobqueue_t *q, const job_t *job)
{
    bool pushed = false;
    pthread_mutex_lock(&q->mutex);
    if (!q->closed && q->count < JOBQ_CAPACITY) {
        int tail = (q->head + q->count) % JOBQ_CAPACITY;
        q->items[tail] = *job;
        q->count++;
        pushed = true;
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->mutex);
    return pushed;
}

bool jobq_pop(jobqueue_t *q, job_t *out)
{
    pthread_mutex_lock(&q->mutex);
    /* "while", not "if": wake-ups can be spurious, and another worker may
     * have taken the job first. */
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    bool got = false;
    if (q->count > 0) {          /* jobs are still handed out after close */
        *out = q->items[q->head];
        q->head = (q->head + 1) % JOBQ_CAPACITY;
        q->count--;
        got = true;
    }
    pthread_mutex_unlock(&q->mutex);
    return got;
}

void jobq_close(jobqueue_t *q)
{
    pthread_mutex_lock(&q->mutex);
    q->closed = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}
