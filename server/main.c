/*
 * server/main.c — start-up, threads, event loop, teardown (SPEC §6.1, §6.9).
 *
 *   ./server [--port 5555] [--db chat.db] [--users users.txt]
 *
 * Threads (always 7): this main thread (becomes the event loop), 4 workers,
 * 1 expiry thread, 1 signal thread.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "eventloop.h"
#include "expiry.h"
#include "jobqueue.h"
#include "log.h"
#include "net.h"
#include "session.h"
#include "signals.h"
#include "users.h"
#include "workers.h"

static void usage(void)
{
    fprintf(stderr, "usage: ./server [--port 5555] [--db chat.db] [--users users.txt]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;
    const char *db_path = "chat.db";
    const char *users_path = "users.txt";
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc)
            usage();
        if (strcmp(argv[i], "--port") == 0)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--db") == 0)
            db_path = argv[++i];
        else if (strcmp(argv[i], "--users") == 0)
            users_path = argv[++i];
        else
            usage();
    }
    if (port <= 0 || port > 65535)
        usage();

    /* Must come before any pthread_create: every thread inherits the mask. */
    signals_block();

    char errbuf[256];
    if (users_load(users_path, errbuf, sizeof errbuf) != 0) {
        fprintf(stderr, "error: %s\n", errbuf);
        return 1;
    }
    db_err_t dberr;
    if (db_open(db_path, &dberr) != DB_OK) {
        fprintf(stderr, "error: database %s: %s\n", db_path, dberr.msg);
        return 1;
    }
    int listen_fd = tcp_listen(port);
    if (listen_fd < 0) {
        perror("error: cannot listen");
        db_close();
        return 1;
    }
    set_nonblocking(listen_fd);

    sessions_init();
    if (eventloop_init(listen_fd) != 0) {
        perror("error: pipe");
        return 1;
    }
    jobqueue_t jobs;
    jobq_init(&jobs);

    atomic_store(&g_producers_alive, NUM_WORKERS + 1);   /* workers + expiry thread */
    workers_start(&jobs);
    expiry_start();
    signals_start();

    log_event("server listening on 0.0.0.0:%d (%d slots, db=%s, %d users)", port, MAX_CLIENTS,
              db_path, users_count());

    eventloop_run(&jobs);   /* returns after the shutdown sequence (eventloop.h) */

    /* Every producer has already exited, so these joins return at once. */
    workers_join();
    expiry_join();
    signals_stop_and_join();
    eventloop_cleanup();

    /* The workers drained the queue; free anything left just in case. */
    job_t leftover;
    jobq_close(&jobs);
    while (jobq_pop(&jobs, &leftover))
        free(leftover.req);
    jobq_destroy(&jobs);

    sessions_destroy();
    db_close();
    users_free();
    log_event("server stopped");
    return 0;
}
