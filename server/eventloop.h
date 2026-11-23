/*
 * eventloop.h — the poll() loop: the ONLY code that touches sockets (SPEC §6.3).
 *
 * The main thread becomes the event loop after start-up. Each iteration it
 * polls the listening socket, the wake pipe and every client socket, then:
 * accepts connections, reads frames into each client's inbox, writes output
 * buffers, hands frames to the workers (one job per client at a time, so each
 * client's commands run in order), and closes / frees finished sessions.
 *
 * The wake pipe: other threads cannot interrupt poll() directly, so after
 * they change something the event loop must react to (new output, a job
 * finished, shutdown) they write one byte into a pipe that poll() watches.
 */
#ifndef EVENTLOOP_H
#define EVENTLOOP_H

#include <stdatomic.h>

#include "jobqueue.h"

#define INBOX_CAP 16   /* decoded frames waiting per client (backpressure) */

/* Threads that can still produce output frames (4 workers + the expiry
 * thread). Each decrements it as its very last action. During shutdown the
 * event loop waits for 0 before sending the final SHUTDOWN frame. */
extern atomic_int g_producers_alive;

/* Creates the wake pipe and remembers the listening socket. Returns 0 / -1. */
int eventloop_init(int listen_fd);

/* Wakes the event loop from poll(). Safe from any thread; never blocks. */
void eventloop_wake(void);

/* Runs until shutdown is complete (SPEC §6.9 + Rev 3.1 errata):
 *   RUNNING  -> normal operation
 *   DRAINING -> listener closed, no new jobs, expiry stopped, job queue
 *               closed; output keeps flowing while workers finish
 *   FLUSHING -> every producer has exited; SHUTDOWN appended as the final
 *               frame of every client; flush for at most 2 s
 *   DONE     -> every socket closed; return to main */
void eventloop_run(jobqueue_t *q);

/* Closes the wake pipe. Call only after every other thread has been joined
 * (any of them may still call eventloop_wake until it exits). */
void eventloop_cleanup(void);

#endif
