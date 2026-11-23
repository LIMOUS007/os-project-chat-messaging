/*
 * session.c — the shared session table. See session.h for the rules.
 */
#include "session.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t        gen;          /* bumped every time the slot is freed          */
    session_state_t state;
    char            username[NAME_BUF];
    time_t          accepted_at;  /* for the 10 s login timeout                    */
    bool            busy;         /* a job for this session is queued or running   */
    close_req_t     close_req;    /* close requested by a worker                   */
    uint8_t        *outbuf;       /* encoded frames waiting to be written          */
    size_t          out_used;     /* bytes in outbuf                               */
    size_t          out_cap;      /* allocated size of outbuf                      */
} session_t;

/* sessions_mutex: the event loop, the 4 workers and the expiry thread all read
 * and change this table (state, busy, outbufs), so every access is under it. */
static session_t       sessions[MAX_CLIENTS];
static pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

void sessions_init(void)
{
    memset(sessions, 0, sizeof sessions);
    for (int i = 0; i < MAX_CLIENTS; i++)
        sessions[i].state = S_FREE;
}

void sessions_destroy(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        free(sessions[i].outbuf);
        sessions[i].outbuf = NULL;
        sessions[i].out_used = sessions[i].out_cap = 0;
    }
}

/* ---------- helpers: caller already holds sessions_mutex ---------- */

static bool handle_is_current(session_handle_t h)
{
    return h.slot < MAX_CLIENTS && sessions[h.slot].gen == h.gen;
}

static bool can_receive(const session_t *s)
{
    return (s->state == S_LOGIN || s->state == S_ACTIVE) && s->close_req == CLOSE_NONE;
}

/* Appends bytes to a session's outbuf, enforcing the 256 KiB limit. */
static send_result_t append_locked(session_t *s, const void *data, size_t n)
{
    if (!can_receive(s))
        return SEND_STALE;
    if (s->out_used + n > OUTBUF_MAX) {
        s->close_req = CLOSE_NOW;   /* slow client: the event loop will close it */
        return SEND_OVERFLOW;
    }
    if (s->out_used + n > s->out_cap) {
        size_t cap = s->out_cap ? s->out_cap : 4096;
        while (cap < s->out_used + n)
            cap *= 2;
        if (cap > OUTBUF_MAX)
            cap = OUTBUF_MAX;
        uint8_t *bigger = realloc(s->outbuf, cap);
        if (bigger == NULL) {
            s->close_req = CLOSE_NOW;
            return SEND_OVERFLOW;
        }
        s->outbuf = bigger;
        s->out_cap = cap;
    }
    memcpy(s->outbuf + s->out_used, data, n);
    s->out_used += n;
    return SEND_OK;
}

/* ---------- workers / expiry ---------- */

send_result_t session_send(session_handle_t h, const void *data, size_t n)
{
    send_result_t r = SEND_STALE;
    pthread_mutex_lock(&sessions_mutex);
    if (handle_is_current(h))
        r = append_locked(&sessions[h.slot], data, n);
    pthread_mutex_unlock(&sessions_mutex);
    return r;
}

send_result_t session_send_to_user(const char *username, const void *data, size_t n)
{
    send_result_t r = SEND_STALE;
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].state == S_ACTIVE && strcmp(sessions[i].username, username) == 0) {
            r = append_locked(&sessions[i], data, n);
            break;
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
    return r;
}

activate_result_t session_try_activate(session_handle_t h, const char *username,
                                       const void *login_ok, size_t n)
{
    activate_result_t r = ACTIVATE_OK;
    pthread_mutex_lock(&sessions_mutex);
    if (!handle_is_current(h) || sessions[h.slot].state != S_LOGIN ||
        sessions[h.slot].close_req != CLOSE_NONE) {
        r = ACTIVATE_STALE;
    } else {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (sessions[i].state == S_ACTIVE && strcmp(sessions[i].username, username) == 0) {
                r = ACTIVATE_DUPLICATE;
                break;
            }
        }
        if (r == ACTIVATE_OK) {
            session_t *s = &sessions[h.slot];
            s->state = S_ACTIVE;
            strncpy(s->username, username, NAME_BUF - 1);
            s->username[NAME_BUF - 1] = '\0';
            append_locked(s, login_ok, n);  /* outbuf is empty: cannot overflow */
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
    return r;
}

void session_request_close(session_handle_t h, close_req_t kind)
{
    pthread_mutex_lock(&sessions_mutex);
    if (handle_is_current(h) && kind > sessions[h.slot].close_req)
        sessions[h.slot].close_req = kind;
    pthread_mutex_unlock(&sessions_mutex);
}

void session_job_done(session_handle_t h)
{
    pthread_mutex_lock(&sessions_mutex);
    if (handle_is_current(h))
        sessions[h.slot].busy = false;
    pthread_mutex_unlock(&sessions_mutex);
}

static int compare_names(const void *a, const void *b) { return strcmp(a, b); }

int session_online_list(char (*names)[NAME_BUF], int max)
{
    int n = 0;
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_CLIENTS && n < max; i++) {
        if (sessions[i].state == S_ACTIVE)
            memcpy(names[n++], sessions[i].username, NAME_BUF);
    }
    pthread_mutex_unlock(&sessions_mutex);
    qsort(names, (size_t)n, NAME_BUF, compare_names);
    return n;
}

/* ---------- event loop only ---------- */

bool session_alloc(session_handle_t *out)
{
    bool ok = false;
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        session_t *s = &sessions[i];
        if (s->state == S_FREE) {
            s->state = S_LOGIN;
            s->username[0] = '\0';
            s->accepted_at = time(NULL);
            s->busy = false;
            s->close_req = CLOSE_NONE;
            s->out_used = 0;
            out->slot = (uint32_t)i;
            out->gen = s->gen;
            ok = true;
            break;
        }
    }
    pthread_mutex_unlock(&sessions_mutex);
    return ok;
}

session_info_t session_get_info(int slot)
{
    session_info_t info;
    pthread_mutex_lock(&sessions_mutex);
    session_t *s = &sessions[slot];
    info.state = s->state;
    info.gen = s->gen;
    info.busy = s->busy;
    info.close_req = s->close_req;
    info.accepted_at = s->accepted_at;
    info.out_pending = s->out_used;
    memcpy(info.username, s->username, NAME_BUF);
    pthread_mutex_unlock(&sessions_mutex);
    return info;
}

bool session_reserve_job(int slot, session_handle_t *h, char *username_out)
{
    bool ok = false;
    pthread_mutex_lock(&sessions_mutex);
    session_t *s = &sessions[slot];
    if (can_receive(s) && !s->busy) {
        s->busy = true;               /* reserved BEFORE the job is published */
        h->slot = (uint32_t)slot;
        h->gen = s->gen;
        memcpy(username_out, s->username, NAME_BUF);
        ok = true;
    }
    pthread_mutex_unlock(&sessions_mutex);
    return ok;
}

void session_unreserve(int slot)
{
    pthread_mutex_lock(&sessions_mutex);
    sessions[slot].busy = false;
    pthread_mutex_unlock(&sessions_mutex);
}

size_t session_outbuf_peek(int slot, void *buf, size_t max)
{
    pthread_mutex_lock(&sessions_mutex);
    session_t *s = &sessions[slot];
    size_t n = s->out_used < max ? s->out_used : max;
    memcpy(buf, s->outbuf, n);
    pthread_mutex_unlock(&sessions_mutex);
    return n;
}

void session_outbuf_consume(int slot, size_t n)
{
    pthread_mutex_lock(&sessions_mutex);
    session_t *s = &sessions[slot];
    if (n > s->out_used)
        n = s->out_used;
    memmove(s->outbuf, s->outbuf + n, s->out_used - n);
    s->out_used -= n;
    pthread_mutex_unlock(&sessions_mutex);
}

void session_mark_closing(int slot, char *username_out)
{
    pthread_mutex_lock(&sessions_mutex);
    session_t *s = &sessions[slot];
    s->state = S_CLOSING;
    s->out_used = 0;
    s->close_req = CLOSE_NONE;
    memcpy(username_out, s->username, NAME_BUF);
    pthread_mutex_unlock(&sessions_mutex);
}

bool session_free_if_idle(int slot)
{
    bool freed = false;
    pthread_mutex_lock(&sessions_mutex);
    session_t *s = &sessions[slot];
    if (s->state == S_CLOSING && !s->busy) {
        s->gen++;                     /* old handles now fail harmlessly */
        s->state = S_FREE;
        s->username[0] = '\0';
        freed = true;
    }
    pthread_mutex_unlock(&sessions_mutex);
    return freed;
}

int session_broadcast(const void *data, size_t n)
{
    int count = 0;
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (can_receive(&sessions[i]) && append_locked(&sessions[i], data, n) == SEND_OK)
            count++;
    }
    pthread_mutex_unlock(&sessions_mutex);
    return count;
}

bool session_all_flushed(void)
{
    bool flushed = true;
    pthread_mutex_lock(&sessions_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        session_state_t st = sessions[i].state;
        if ((st == S_LOGIN || st == S_ACTIVE) && sessions[i].out_used > 0)
            flushed = false;
    }
    pthread_mutex_unlock(&sessions_mutex);
    return flushed;
}
