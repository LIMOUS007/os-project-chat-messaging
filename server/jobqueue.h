/*
 * jobqueue.h — bounded producer-consumer queue of jobs (SPEC §6.4).
 *
 * Producer: the event loop, with jobq_try_push only (it must never block).
 * Consumers: the 4 workers, with jobq_pop (blocks until there is a job).
 * Each queue has its own mutex + condition variable; no other lock is ever
 * held together with it.
 */
#ifndef JOBQUEUE_H
#define JOBQUEUE_H

#include <pthread.h>
#include <stdbool.h>

#include "protocol.h"
#include "session.h"

#define JOBQ_CAPACITY 64

/* One received frame to be handled by a worker. */
typedef struct {
    session_handle_t h;                   /* which session sent it          */
    char             username[NAME_BUF];  /* copied: never a pointer into the table;
                                             "" while not logged in         */
    frame_t         *req;                 /* the request; once queued, the job
                                             owns it and the worker frees it */
} job_t;

typedef struct {
    job_t           items[JOBQ_CAPACITY]; /* ring buffer                     */
    int             head, count;          /* index of oldest item, items held */
    bool            closed;               /* no more pushes; pop drains then NULL */
    pthread_mutex_t mutex;                /* protects everything above       */
    pthread_cond_t  not_empty;            /* signalled on push and on close  */
} jobqueue_t;

void jobq_init(jobqueue_t *q);
void jobq_destroy(jobqueue_t *q);

/* Copies *job into the queue if there is room and the queue is open.
 * Never blocks. Returns false if full or closed. Locks: q->mutex. */
bool jobq_try_push(jobqueue_t *q, const job_t *job);

/* Waits for a job and copies it into *out. Returns false once the queue is
 * closed AND empty — that is how workers know to exit. Locks: q->mutex. */
bool jobq_pop(jobqueue_t *q, job_t *out);

/* Closes the queue and wakes every waiting consumer. Locks: q->mutex. */
void jobq_close(jobqueue_t *q);

#endif
