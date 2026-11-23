/*
 * workers.c — the worker pool.
 */
#include "workers.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "eventloop.h"
#include "handlers.h"
#include "session.h"

static pthread_t workers[NUM_WORKERS];

static void *worker_main(void *arg)
{
    jobqueue_t *q = arg;
    job_t job;
    while (jobq_pop(q, &job)) {     /* false = queue closed and empty: exit */
        handle_job(&job);
        free(job.req);              /* the job owned the request frame */
        session_job_done(job.h);    /* busy = false (if the slot was not reused) */
        eventloop_wake();           /* new output to write / next frame to dispatch */
    }
    /* Last action: tell the event loop one fewer frame producer is alive. */
    atomic_fetch_sub(&g_producers_alive, 1);
    eventloop_wake();
    return NULL;
}

void workers_start(jobqueue_t *q)
{
    for (int i = 0; i < NUM_WORKERS; i++)
        pthread_create(&workers[i], NULL, worker_main, q);
}

void workers_join(void)
{
    for (int i = 0; i < NUM_WORKERS; i++)
        pthread_join(workers[i], NULL);
}
