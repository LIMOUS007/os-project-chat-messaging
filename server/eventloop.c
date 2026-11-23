/*
 * eventloop.c — the server's I/O thread. See eventloop.h.
 */
#include "eventloop.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "expiry.h"
#include "log.h"
#include "net.h"
#include "protocol.h"
#include "session.h"
#include "signals.h"

#define LOGIN_TIMEOUT_SEC  10
#define FLUSH_DEADLINE_MS  2000
#define DRAIN_WARNING_MS   5000
#define WRITE_CHUNK        (16 * 1024)

atomic_int g_producers_alive = 0;

/* Per-connection data that ONLY the event loop thread touches, so it needs no
 * lock. (Shared per-session data lives in session.c under sessions_mutex.) */
typedef struct {
    int            fd;                    /* -1 = no socket                 */
    frame_parser_t parser;                /* partial incoming frame         */
    frame_t       *inbox[INBOX_CAP];      /* complete frames not yet dispatched */
    int            inbox_head, inbox_len;
} conn_t;

typedef enum { PHASE_RUNNING, PHASE_DRAINING, PHASE_FLUSHING, PHASE_DONE } phase_t;

static conn_t conns[MAX_CLIENTS];
static int    listen_fd = -1;
static int    wake_pipe[2] = { -1, -1 };

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int eventloop_init(int fd)
{
    listen_fd = fd;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        conns[i].fd = -1;
        conns[i].inbox_head = conns[i].inbox_len = 0;
    }
    if (pipe(wake_pipe) < 0)
        return -1;
    set_nonblocking(wake_pipe[0]);
    set_nonblocking(wake_pipe[1]);   /* a full pipe is fine: one wake-up is enough */
    return 0;
}

void eventloop_wake(void)
{
    char byte = 1;
    ssize_t ignored = write(wake_pipe[1], &byte, 1);
    (void)ignored;
}

static void drain_wake_pipe(void)
{
    char buf[256];
    while (read(wake_pipe[0], buf, sizeof buf) > 0) {
    }
}

/* ======================= inbox ======================= */

static void inbox_push(conn_t *c, frame_t *f)
{
    c->inbox[(c->inbox_head + c->inbox_len) % INBOX_CAP] = f;
    c->inbox_len++;
}

/* Frees the frames the event loop still owns (never dispatched). */
static void inbox_discard(conn_t *c)
{
    while (c->inbox_len > 0) {
        free(c->inbox[c->inbox_head]);
        c->inbox_head = (c->inbox_head + 1) % INBOX_CAP;
        c->inbox_len--;
    }
}

/* ======================= connections ======================= */

/* Closes a client connection (SPEC §6.6 step 3). The slot becomes CLOSING
 * and is freed later, once no job refers to it any more. */
static void close_conn(int slot, const char *reason)
{
    conn_t *c = &conns[slot];
    char name[NAME_BUF];
    session_mark_closing(slot, name);
    inbox_discard(c);
    close(c->fd);
    c->fd = -1;
    /* Logged after session_mark_closing returned: no lock is held here. */
    if (name[0] != '\0')
        log_event("%s disconnected%s%s%s", name, reason ? " (" : "", reason ? reason : "",
                  reason ? ")" : "");
    else
        log_event("connection on slot %d closed%s%s%s", slot, reason ? " (" : "",
                  reason ? reason : "", reason ? ")" : "");
}

static void accept_clients(void)
{
    for (;;) {
        struct sockaddr_in addr;
        socklen_t len = sizeof addr;
        int fd = accept(listen_fd, (struct sockaddr *)&addr, &len);
        if (fd < 0)
            return;                    /* EAGAIN: no more pending connections */

        char peer[64];
        snprintf(peer, sizeof peer, "%s:%d", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
        set_nonblocking(fd);

        session_handle_t h;
        if (!session_alloc(&h)) {
            /* Table full (R2): one non-blocking attempt to say so, then close. */
            frame_builder_t fb;
            enc_error(&fb, E_SERVER_FULL, "server is full");
            ssize_t ignored = send(fd, fb.buf, fb.used, MSG_NOSIGNAL | MSG_DONTWAIT);
            (void)ignored;
            close(fd);
            log_event("connection from %s refused: server full", peer);
            continue;
        }
        conn_t *c = &conns[h.slot];
        c->fd = fd;
        c->inbox_head = c->inbox_len = 0;
        parser_reset(&c->parser);
        log_event("connection from %s (slot %u)", peer, h.slot);
    }
}

/* Reads frames into the inbox until it is full or no data is left. The
 * parser never reads beyond the current frame, so once the inbox is full the
 * remaining bytes wait in the kernel and TCP slows the sender (N3). */
static void read_from_client(int slot)
{
    conn_t *c = &conns[slot];
    while (c->inbox_len < INBOX_CAP) {
        frame_t *f;
        parse_result_t r = parser_read(&c->parser, c->fd, &f);
        if (r == PARSE_FRAME) {
            inbox_push(c, f);
            continue;
        }
        if (r == PARSE_AGAIN)
            return;
        close_conn(slot, r == PARSE_CLOSED ? NULL : "socket or framing error");
        return;
    }
}

/* Writes part of the output buffer (SPEC §6.7): copy under the lock, send
 * with NO lock held, then remove what was actually sent. */
static void write_to_client(int slot)
{
    uint8_t chunk[WRITE_CHUNK];
    size_t n = session_outbuf_peek(slot, chunk, sizeof chunk);
    if (n == 0)
        return;
    ssize_t sent = send(conns[slot].fd, chunk, n, MSG_NOSIGNAL);
    if (sent > 0)
        session_outbuf_consume(slot, (size_t)sent);
    else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        close_conn(slot, "send error");
}

/* ======================= dispatch ======================= */

/* Hands the head frame of each idle session to the workers (Rev 3.1 [A]):
 *   1. reserve: busy = true (under sessions_mutex)
 *   2. publish: jobq_try_push (never blocks)
 *   3. success: ownership of the frame moved to the job — we only drop our
 *      pointer to it, we never touch or free it again.
 *      failure: roll back busy = false; the inbox keeps the frame. */
static void dispatch(jobqueue_t *q)
{
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
        conn_t *c = &conns[slot];
        if (c->fd < 0 || c->inbox_len == 0)
            continue;
        job_t job;
        if (!session_reserve_job(slot, &job.h, job.username))
            continue;                       /* busy, closing, or close requested */
        job.req = c->inbox[c->inbox_head];
        if (jobq_try_push(q, &job)) {
            c->inbox[c->inbox_head] = NULL;
            c->inbox_head = (c->inbox_head + 1) % INBOX_CAP;
            c->inbox_len--;
        } else {
            session_unreserve(slot);        /* queue full: retry next iteration */
            return;
        }
    }
}

/* ======================= housekeeping ======================= */

/* Carries out close requests, the login timeout, and frees CLOSING slots
 * that no job refers to any more (SPEC §6.6 step 5). */
static void housekeeping(void)
{
    time_t now = time(NULL);
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
        session_info_t info = session_get_info(slot);
        bool open = conns[slot].fd >= 0 && (info.state == S_LOGIN || info.state == S_ACTIVE);
        if (open) {
            if (info.close_req == CLOSE_NOW)
                close_conn(slot, NULL);
            else if (info.close_req == CLOSE_AFTER_FLUSH && info.out_pending == 0)
                close_conn(slot, NULL);
            else if (info.state == S_LOGIN && !info.busy &&
                     now - info.accepted_at >= LOGIN_TIMEOUT_SEC)
                close_conn(slot, "login timeout");
        }
        session_free_if_idle(slot);   /* only acts on CLOSING slots that are not busy */
    }
}

/* ======================= one poll() round ======================= */

static void poll_once(phase_t phase)
{
    struct pollfd pfds[2 + MAX_CLIENTS];
    int slot_of[2 + MAX_CLIENTS];
    int n = 0;

    pfds[n].fd = wake_pipe[0];
    pfds[n].events = POLLIN;
    slot_of[n++] = -1;
    if (phase == PHASE_RUNNING && listen_fd >= 0) {
        pfds[n].fd = listen_fd;
        pfds[n].events = POLLIN;
        slot_of[n++] = -2;
    }
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
        conn_t *c = &conns[slot];
        if (c->fd < 0)
            continue;
        session_info_t info = session_get_info(slot);
        short events = 0;
        if (phase == PHASE_RUNNING && c->inbox_len < INBOX_CAP && info.close_req == CLOSE_NONE)
            events |= POLLIN;        /* inbox full -> stop reading (backpressure) */
        if (info.out_pending > 0)
            events |= POLLOUT;
        if (events == 0)
            continue;
        pfds[n].fd = c->fd;
        pfds[n].events = events;
        slot_of[n++] = slot;
    }

    int timeout = phase == PHASE_RUNNING ? 1000 : 100;
    if (poll(pfds, (nfds_t)n, timeout) <= 0)
        return;                      /* timeout (housekeeping still runs) or EINTR */

    for (int i = 0; i < n; i++) {
        short rev = pfds[i].revents;
        if (rev == 0)
            continue;
        if (slot_of[i] == -1) {
            drain_wake_pipe();
            continue;
        }
        if (slot_of[i] == -2) {
            accept_clients();
            continue;
        }
        int slot = slot_of[i];
        if (rev & POLLIN)
            read_from_client(slot);
        if (conns[slot].fd >= 0 && (rev & POLLOUT))
            write_to_client(slot);
        if (conns[slot].fd >= 0 && ((rev & (POLLERR | POLLNVAL)) ||
                                    ((rev & POLLHUP) && !(rev & POLLIN))))
            close_conn(slot, "connection error");
    }
}

/* ======================= shutdown phases ======================= */

/* RUNNING -> DRAINING: stop all new work and stop the frame producers. */
static void start_draining(jobqueue_t *q)
{
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    for (int slot = 0; slot < MAX_CLIENTS; slot++)
        inbox_discard(&conns[slot]);       /* requests that were never dispatched */
    expiry_stop();
    jobq_close(q);                         /* workers finish what is queued, then exit */
}

/* DRAINING -> FLUSHING: no thread can produce frames any more, so SHUTDOWN
 * is guaranteed to be the last frame every client receives. */
static void start_flushing(void)
{
    frame_builder_t fb;
    enc_text_frame(&fb, T_SHUTDOWN, "Server shutting down...");
    int n = session_broadcast(fb.buf, fb.used);
    log_event("shutdown requested (%s) — notifying %d clients", shutdown_signal_name(), n);
}

void eventloop_run(jobqueue_t *q)
{
    phase_t phase = PHASE_RUNNING;
    long long drain_started = 0, flush_deadline = 0;
    bool warned = false;

    while (phase != PHASE_DONE) {
        long long now = now_ms();
        if (phase == PHASE_RUNNING && shutdown_requested()) {
            start_draining(q);
            phase = PHASE_DRAINING;
            drain_started = now;
        }
        if (phase == PHASE_DRAINING) {
            if (atomic_load(&g_producers_alive) == 0) {
                start_flushing();
                phase = PHASE_FLUSHING;
                flush_deadline = now + FLUSH_DEADLINE_MS;
            } else if (!warned && now - drain_started >= DRAIN_WARNING_MS) {
                log_event("warning: still waiting for workers to finish");
                warned = true;     /* keep waiting: there is no forced exit */
            }
        }
        if (phase == PHASE_FLUSHING && (session_all_flushed() || now >= flush_deadline)) {
            phase = PHASE_DONE;
            break;
        }

        poll_once(phase);
        if (phase == PHASE_RUNNING)
            dispatch(q);
        housekeeping();
    }

    /* DONE: close every remaining connection. */
    for (int slot = 0; slot < MAX_CLIENTS; slot++) {
        if (conns[slot].fd >= 0)
            close_conn(slot, NULL);
        session_free_if_idle(slot);
    }
}

void eventloop_cleanup(void)
{
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    wake_pipe[0] = wake_pipe[1] = -1;
}
