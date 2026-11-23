/*
 * test_session.c — U5: session table rules.
 *   - duplicate login is rejected
 *   - session_send with a stale generation is dropped
 *   - output-buffer overflow requests a close (state unchanged) and drops
 *   - after a close request, further sends are dropped
 *   - dispatch race (Rev 3.1 [A]): reserve busy -> push -> rollback, against
 *     workers clearing busy, never leaves busy stuck and never runs two jobs
 *     of one session at once
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "jobqueue.h"
#include "session.h"
#include "test.h"

static void test_duplicate_login(void)
{
    sessions_init();
    session_handle_t a, b;
    CHECK(session_alloc(&a));
    CHECK(session_alloc(&b));
    const char hello[] = "x";
    CHECK_EQ(session_try_activate(a, "alice", hello, 1), ACTIVATE_OK);
    CHECK_EQ(session_try_activate(b, "alice", hello, 1), ACTIVATE_DUPLICATE);
    CHECK_EQ(session_get_info((int)a.slot).state, S_ACTIVE);
    CHECK_EQ(session_get_info((int)b.slot).state, S_LOGIN);
    CHECK_EQ(session_try_activate(b, "bob", hello, 1), ACTIVATE_OK);

    char names[MAX_CLIENTS][NAME_BUF];
    CHECK_EQ(session_online_list(names, MAX_CLIENTS), 2);
    CHECK(strcmp(names[0], "alice") == 0 && strcmp(names[1], "bob") == 0);
    sessions_destroy();
}

static void test_stale_generation(void)
{
    sessions_init();
    session_handle_t h;
    CHECK(session_alloc(&h));
    session_try_activate(h, "alice", "x", 1);
    char name[NAME_BUF];
    session_mark_closing((int)h.slot, name);
    CHECK(session_free_if_idle((int)h.slot));       /* gen bumped, slot FREE */

    session_handle_t reused;
    CHECK(session_alloc(&reused));
    CHECK_EQ(reused.slot, h.slot);                  /* same slot ... */
    CHECK(reused.gen != h.gen);                     /* ... new generation */
    CHECK_EQ(session_send(h, "old", 3), SEND_STALE);
    CHECK_EQ(session_get_info((int)reused.slot).out_pending, 0);
    CHECK_EQ(session_send(reused, "new", 3), SEND_OK);
    CHECK_EQ(session_get_info((int)reused.slot).out_pending, 3);
    session_job_done(h);                            /* stale: no effect */
    sessions_destroy();
}

static void test_overflow_and_close_requests(void)
{
    sessions_init();
    session_handle_t h;
    session_alloc(&h);
    session_try_activate(h, "alice", "x", 1);
    static char chunk[64 * 1024];
    int ok = 0;
    send_result_t r;
    while ((r = session_send(h, chunk, sizeof chunk)) == SEND_OK)
        ok++;
    CHECK_EQ(r, SEND_OVERFLOW);
    CHECK_EQ(ok, 3);                                /* 1 + 3 * 64 KiB fits in 256 KiB */
    session_info_t info = session_get_info((int)h.slot);
    CHECK_EQ(info.close_req, CLOSE_NOW);            /* close requested ...        */
    CHECK_EQ(info.state, S_ACTIVE);                 /* ... but state unchanged     */
    CHECK_EQ(session_send(h, "x", 1), SEND_STALE);  /* later sends are dropped     */

    /* CLOSE_AFTER_FLUSH also drops anything sent afterwards. */
    session_handle_t g;
    session_alloc(&g);
    CHECK_EQ(session_send(g, "err", 3), SEND_OK);
    session_request_close(g, CLOSE_AFTER_FLUSH);
    CHECK_EQ(session_send(g, "more", 4), SEND_STALE);
    CHECK_EQ(session_get_info((int)g.slot).out_pending, 3);
    session_request_close(g, CLOSE_NOW);            /* stronger request wins */
    CHECK_EQ(session_get_info((int)g.slot).close_req, CLOSE_NOW);
    session_request_close(g, CLOSE_AFTER_FLUSH);    /* weaker one does not undo it */
    CHECK_EQ(session_get_info((int)g.slot).close_req, CLOSE_NOW);
    sessions_destroy();
}

/* ---- dispatch race ---- */

#define RACE_SESSIONS 8
#define RACE_JOBS     20000

static jobqueue_t q;
static atomic_int in_flight[RACE_SESSIONS];
static atomic_int overlap_errors;
static atomic_int processed;

static void *fake_worker(void *arg)
{
    (void)arg;
    job_t job;
    while (jobq_pop(&q, &job)) {
        if (atomic_fetch_add(&in_flight[job.h.slot], 1) != 0)
            atomic_fetch_add(&overlap_errors, 1);   /* two jobs of one session at once */
        sched_yield();
        atomic_fetch_sub(&in_flight[job.h.slot], 1);
        atomic_fetch_add(&processed, 1);
        session_job_done(job.h);                    /* busy = false */
    }
    return NULL;
}

static void test_dispatch_race(void)
{
    sessions_init();
    jobq_init(&q);
    for (int i = 0; i < RACE_SESSIONS; i++) {
        session_handle_t h;
        session_alloc(&h);
    }
    pthread_t workers[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&workers[i], NULL, fake_worker, NULL);

    /* The "event loop": exactly the dispatch steps of eventloop.c. */
    int pushed = 0;
    while (pushed < RACE_JOBS) {
        for (int slot = 0; slot < RACE_SESSIONS && pushed < RACE_JOBS; slot++) {
            job_t job;
            if (!session_reserve_job(slot, &job.h, job.username))
                continue;
            job.req = NULL;
            if (jobq_try_push(&q, &job))
                pushed++;
            else
                session_unreserve(slot);
        }
    }
    jobq_close(&q);
    for (int i = 0; i < 4; i++)
        pthread_join(workers[i], NULL);

    CHECK_EQ(atomic_load(&processed), RACE_JOBS);
    CHECK_EQ(atomic_load(&overlap_errors), 0);
    int stuck = 0;
    for (int slot = 0; slot < RACE_SESSIONS; slot++)
        if (session_get_info(slot).busy)
            stuck++;
    CHECK_EQ(stuck, 0);                             /* busy never left set */
    jobq_destroy(&q);
    sessions_destroy();
}

int main(void)
{
    test_duplicate_login();
    test_stale_generation();
    test_overflow_and_close_requests();
    test_dispatch_race();
    return TEST_SUMMARY();
}
