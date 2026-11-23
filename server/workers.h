/*
 * workers.h — the pool of 4 worker threads (SPEC §6.5).
 *
 * Each worker loops: pop a job, run its handler (database work, then append
 * replies to output buffers), free the request, clear the session's busy flag,
 * wake the event loop. Workers never touch sockets.
 */
#ifndef WORKERS_H
#define WORKERS_H

#include "jobqueue.h"

#define NUM_WORKERS 4

/* Starts NUM_WORKERS threads consuming from q. */
void workers_start(jobqueue_t *q);

/* Joins all workers. They exit once q is closed and empty. */
void workers_join(void);

#endif
