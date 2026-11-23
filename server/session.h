/*
 * session.h — the session table: one slot per connected client (SPEC §6.2, §6.6).
 *
 * This module holds the session state that is SHARED between threads, and
 * every function here takes sessions_mutex internally (never two locks).
 *
 *   Shared (here, under sessions_mutex): gen, state, username, accepted_at,
 *       busy, close_req, outbuf.
 *   Private to the event loop (in eventloop.c, no lock): the socket fd, the
 *       incremental frame parser and the inbox of received frames.
 *
 * Ownership rule (Rev 3.1 errata): only the EVENT LOOP moves a slot through
 * FREE -> LOGIN -> CLOSING -> FREE and frees it. Workers may only
 *   - switch LOGIN -> ACTIVE (session_try_activate: the duplicate-login check),
 *   - clear busy (session_job_done),
 *   - append to an outbuf (session_send*),
 *   - REQUEST a close (session_request_close); the event loop then closes it.
 *
 * Handles: a slot number plus the slot's generation counter. The counter is
 * bumped every time a slot is freed, so a handle to an old, reused slot no
 * longer matches and every operation on it is silently ignored.
 */
#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "protocol.h"

#ifndef MAX_CLIENTS
#define MAX_CLIENTS 32          /* hard cap (R1); test builds use 6 */
#endif
#define OUTBUF_MAX (256 * 1024) /* per-session output buffer limit (N3) */

typedef struct {
    uint32_t slot, gen;
} session_handle_t;

typedef enum { S_FREE, S_LOGIN, S_ACTIVE, S_CLOSING } session_state_t;

/* A close requested by a worker; carried out later by the event loop. */
typedef enum {
    CLOSE_NONE        = 0,
    CLOSE_AFTER_FLUSH = 1,  /* close once the outbuf is empty (failed login) */
    CLOSE_NOW         = 2,  /* close immediately (LOGOUT, outbuf overflow)  */
} close_req_t;

typedef enum {
    SEND_OK,        /* frame appended                                   */
    SEND_STALE,     /* old handle / not LOGIN or ACTIVE / closing: dropped */
    SEND_OVERFLOW,  /* outbuf would exceed 256 KiB: dropped, close requested */
} send_result_t;

typedef enum { ACTIVATE_OK, ACTIVATE_DUPLICATE, ACTIVATE_STALE } activate_result_t;

/* A copy of one slot's shared fields, for the event loop's decisions. */
typedef struct {
    session_state_t state;
    uint32_t        gen;
    bool            busy;
    close_req_t     close_req;
    time_t          accepted_at;
    size_t          out_pending;      /* bytes waiting in the outbuf */
    char            username[NAME_BUF];
} session_info_t;

/* Initialises the table (all FREE). Call once before any thread starts. */
void sessions_init(void);

/* Frees every outbuf. Call once after all threads have stopped. */
void sessions_destroy(void);

/* ---------- used by workers and the expiry thread ---------- */

/* Appends an encoded frame to h's outbuf if h is current, LOGIN/ACTIVE and has
 * no close request. On overflow it drops the frame and requests CLOSE_NOW.
 * The caller must call eventloop_wake() afterwards. Locks: sessions_mutex. */
send_result_t session_send(session_handle_t h, const void *data, size_t n);

/* Same as session_send, for the ACTIVE session logged in as `username`.
 * Returns SEND_STALE if that user is not online. Locks: sessions_mutex. */
send_result_t session_send_to_user(const char *username, const void *data, size_t n);

/* Login step (SPEC §3.3): in ONE critical section, checks no other ACTIVE
 * session uses `username`, marks h ACTIVE with that name and appends the
 * LOGIN_OK frame — so LOGIN_OK is always before any live message.
 * Locks: sessions_mutex. */
activate_result_t session_try_activate(session_handle_t h, const char *username,
                                       const void *login_ok, size_t n);

/* Records a close request (the stronger request wins). The event loop does
 * the actual close. Locks: sessions_mutex. */
void session_request_close(session_handle_t h, close_req_t kind);

/* Login replay paging (handlers.c): remember that more pending messages
 * remain after message id after_id. Locks: sessions_mutex. */
void session_set_replay(session_handle_t h, uint64_t after_id);

/* If a replay page is still owed AND the outbuf holds less than max_pending
 * bytes, clears the flag, returns true and gives the id to continue after.
 * Locks: sessions_mutex. */
bool session_take_replay(session_handle_t h, size_t max_pending, uint64_t *after_id);

/* Clears busy after a worker finished h's job (only if h is still current).
 * Locks: sessions_mutex. */
void session_job_done(session_handle_t h);

/* Copies the names of all ACTIVE users (sorted) into names. Returns the count.
 * Locks: sessions_mutex. */
int session_online_list(char (*names)[NAME_BUF], int max);

/* ---------- used only by the event loop ---------- */

/* Takes a FREE slot and puts it in LOGIN. Returns false if the table is full.
 * Locks: sessions_mutex. */
bool session_alloc(session_handle_t *out);

/* Copies the shared fields of one slot. Locks: sessions_mutex. */
session_info_t session_get_info(int slot);

/* Dispatch step 1 (Rev 3.1 [A]): if the slot is LOGIN/ACTIVE, not busy and
 * has no close request, sets busy = true BEFORE the job is published and
 * returns its handle and username. Locks: sessions_mutex. */
bool session_reserve_job(int slot, session_handle_t *h, char *username_out);

/* Dispatch rollback: the job could not be queued, so busy = false again.
 * Locks: sessions_mutex. */
void session_unreserve(int slot);

/* Copies up to max bytes from the front of the outbuf (no removal).
 * Locks: sessions_mutex. */
size_t session_outbuf_peek(int slot, void *buf, size_t max);

/* Removes n bytes from the front of the outbuf after they were sent.
 * Locks: sessions_mutex. */
void session_outbuf_consume(int slot, size_t n);

/* Marks a slot CLOSING, empties its outbuf and copies its username out (for
 * the log). The event loop closes the fd itself. Locks: sessions_mutex. */
void session_mark_closing(int slot, char *username_out);

/* Frees a CLOSING slot once no job refers to it (!busy): bumps gen, -> FREE.
 * Returns true if freed. Locks: sessions_mutex. */
bool session_free_if_idle(int slot);

/* Appends a frame to every LOGIN/ACTIVE session without a close request.
 * Returns how many sessions got it. Locks: sessions_mutex. */
int session_broadcast(const void *data, size_t n);

/* True if no LOGIN/ACTIVE session has bytes waiting. Locks: sessions_mutex. */
bool session_all_flushed(void);

#endif
