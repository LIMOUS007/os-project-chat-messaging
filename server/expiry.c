/*
 * expiry.c — deletes disappearing messages at their deadline (F1).
 */
#include "expiry.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>

#include "db.h"
#include "eventloop.h"
#include "log.h"
#include "protocol.h"
#include "session.h"

/* expiry_mutex + expiry_cond: the expiry thread sleeps on the condvar; the
 * SEND_TIMED handler and shutdown wake it. expiry_gen and expiry_stopping are
 * only read or written while holding expiry_mutex. */
static pthread_mutex_t expiry_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  expiry_cond = PTHREAD_COND_INITIALIZER;
static unsigned long   expiry_gen = 0;
static bool            expiry_stopping = false;
static pthread_t       expiry_thread;

/* Sleeps until `next` (0 = no deadline: wait up to an hour), or until the
 * generation counter changes, or until shutdown. Returns true if stopping. */
static bool wait_for_deadline(unsigned long gen_seen, time_t next)
{
    pthread_mutex_lock(&expiry_mutex);
    while (expiry_gen == gen_seen && !expiry_stopping) {
        time_t now = time(NULL);
        if (next != 0 && now >= next)
            break;                                  /* deadline reached */
        struct timespec until = { .tv_sec = next != 0 ? next : now + 3600, .tv_nsec = 0 };
        int rc = pthread_cond_timedwait(&expiry_cond, &expiry_mutex, &until);
        if (rc == ETIMEDOUT && next == 0)
            break;                                  /* hourly re-check */
    }
    bool stopping = expiry_stopping;
    pthread_mutex_unlock(&expiry_mutex);
    return stopping;
}

/* Deletes everything expired by now and notifies online recipients. */
static void sweep(void)
{
    db_messages_t expired = {0};
    db_err_t err;
    if (db_delete_expired(time(NULL), &expired, &err) != DB_OK) {
        log_event("database error (expiry): %s", err.msg);
        return;
    }
    /* db_mutex is released here; only now do we touch sessions (lock rule). */
    for (int i = 0; i < expired.count; i++) {
        db_message_t *m = &expired.items[i];
        frame_builder_t fb;
        enc_expired(&fb, m->id, m->sender);
        if (m->recipient[0] != '\0')
            session_send_to_user(m->recipient, fb.buf, fb.used);
        log_event("message #%llu expired", (unsigned long long)m->id);
    }
    if (expired.count > 0)
        eventloop_wake();
    db_messages_free(&expired);
}

static void *expiry_main(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&expiry_mutex);
        unsigned long gen_seen = expiry_gen;
        pthread_mutex_unlock(&expiry_mutex);

        time_t next;
        db_err_t err;
        if (db_next_expiry(&next, &err) != DB_OK) {
            log_event("database error (expiry): %s", err.msg);
            next = time(NULL) + 5;                  /* try again later */
        }

        if (wait_for_deadline(gen_seen, next))
            break;
        sweep();
    }
    /* Last action: tell the event loop one fewer frame producer is alive. */
    atomic_fetch_sub(&g_producers_alive, 1);
    eventloop_wake();
    return NULL;
}

void expiry_start(void) { pthread_create(&expiry_thread, NULL, expiry_main, NULL); }

void expiry_notify(void)
{
    pthread_mutex_lock(&expiry_mutex);
    expiry_gen++;
    pthread_cond_signal(&expiry_cond);
    pthread_mutex_unlock(&expiry_mutex);
}

void expiry_stop(void)
{
    pthread_mutex_lock(&expiry_mutex);
    expiry_stopping = true;
    pthread_cond_signal(&expiry_cond);
    pthread_mutex_unlock(&expiry_mutex);
}

void expiry_join(void) { pthread_join(expiry_thread, NULL); }
