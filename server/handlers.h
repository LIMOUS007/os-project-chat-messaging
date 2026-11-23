/*
 * handlers.h — one function per request type (SPEC §4.2, §3.3).
 *
 * Every handler follows the same three steps:
 *   1. validate the payload (BAD_REQUEST / TOO_LONG / UNKNOWN_USER ...),
 *   2. do the database work (db_* functions take db_mutex themselves),
 *   3. append replies and pushes (session_* functions take sessions_mutex).
 * Logging happens after the lock-taking calls have returned.
 * Handlers never block on the network: session_send only appends to a buffer.
 */
#ifndef HANDLERS_H
#define HANDLERS_H

#include "jobqueue.h"

/* Runs the handler for job->req. Called by worker threads. Does not free
 * job->req (the worker does). */
void handle_job(const job_t *job);

#endif
