/*
 * handlers.c — command handling on the server (one function per frame type).
 *
 * Pattern of every handler: validate -> database -> replies -> log.
 * See handlers.h. The text of every ERROR is written so the client can print
 * it directly as "Error: <text>".
 */
#include "handlers.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"
#include "expiry.h"
#include "log.h"
#include "protocol.h"
#include "session.h"
#include "users.h"

#define MAX_GROUP_MEMBERS 64

/* ======================= reply helpers ======================= */

static void reply(const job_t *job, const frame_builder_t *fb)
{
    if (!fb->err)
        session_send(job->h, fb->buf, fb->used);
}

static void reply_ok(const job_t *job, uint64_t ref, const char *text)
{
    frame_builder_t fb;
    enc_ok(&fb, ref, text);
    reply(job, &fb);
}

static void reply_error(const job_t *job, uint8_t code, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void reply_error(const job_t *job, uint8_t code, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    frame_builder_t fb;
    enc_error(&fb, code, text);
    reply(job, &fb);
}

/* A database call failed: log it (no lock held now) and answer INTERNAL. */
static void reply_db_error(const job_t *job, const db_err_t *err)
{
    log_event("database error: %s", err->msg);
    reply_error(job, E_INTERNAL, "internal server error");
}

/* Sends an encoded frame to a user if online. Returns true if it was queued. */
static bool push_to_user(const char *user, const frame_builder_t *fb)
{
    return !fb->err && session_send_to_user(user, fb->buf, fb->used) == SEND_OK;
}

/* Answers a group-related database status that is not DB_OK. */
static void reply_group_status(const job_t *job, db_status_t st, const char *group,
                               const db_err_t *err, const char *not_creator_text)
{
    switch (st) {
    case DB_NO_GROUP:    reply_error(job, E_NO_SUCH_GROUP, "no such group %s", group); break;
    case DB_NOT_MEMBER:  reply_error(job, E_NOT_MEMBER, "you are not a member of %s", group); break;
    case DB_NOT_CREATOR: reply_error(job, E_NOT_CREATOR, "%s", not_creator_text); break;
    case DB_EXISTS:      reply_error(job, E_GROUP_EXISTS, "group %s already exists", group); break;
    default:             reply_db_error(job, err); break;
    }
}

/* Checks a DM peer name: valid, known, not the caller. Replies on failure. */
static bool check_dm_peer(const job_t *job, const char *peer)
{
    if (!valid_username(peer)) {
        reply_error(job, E_BAD_REQUEST, "invalid username '%s'", peer);
        return false;
    }
    if (!users_exists(peer)) {
        reply_error(job, E_UNKNOWN_USER, "unknown user '%s'", peer);
        return false;
    }
    if (strcmp(peer, job->username) == 0) {
        reply_error(job, E_SELF_MESSAGE, "cannot message yourself");
        return false;
    }
    return true;
}

/* Reports a malformed payload (BAD_REQUEST, or TOO_LONG for long text). */
static void reply_bad_payload(const job_t *job, const reader_t *r)
{
    if (r->too_long)
        reply_error(job, E_TOO_LONG, "message too long (max %d bytes)", MAX_TEXT);
    else
        reply_error(job, E_BAD_REQUEST, "malformed request");
}

/* ======================= LOGIN ======================= */

/* Login failure: send the error, then let the event loop close the
 * connection once the error has been written (Rev 3.1 errata [C]). */
static void reject_login(const job_t *job, uint8_t code, const char *text)
{
    reply_error(job, code, "%s", text);
    session_request_close(job->h, CLOSE_AFTER_FLUSH);
}

static void handle_login(const job_t *job, reader_t *r)
{
    char name[NAME_BUF];
    rd_str(r, name, sizeof name);
    if (!rd_ok(r) || !valid_username(name)) {
        reject_login(job, E_BAD_REQUEST, "invalid username");
        return;
    }
    if (!users_exists(name)) {
        char text[96];
        snprintf(text, sizeof text, "unknown user '%s'", name);
        reject_login(job, E_UNKNOWN_USER, text);
        log_event("login rejected for '%s': unknown user", name);
        return;
    }

    /* 1. Count unread messages. This is a snapshot: a message stored in the
     *    short gap before step 2 is still delivered (live or in step 3) but is
     *    not included in the number (Rev 3.1 errata). */
    time_t now = time(NULL);
    int n_unread = 0;
    db_err_t err;
    if (db_count_pending(name, now, &n_unread, &err) != DB_OK) {
        log_event("database error: %s", err.msg);
        reject_login(job, E_INTERNAL, "internal server error");
        return;
    }

    /* 2. Duplicate check + ACTIVE + LOGIN_OK, all under sessions_mutex at
     *    once, so LOGIN_OK always comes before any live message. */
    frame_builder_t fb;
    enc_login_ok(&fb, (uint32_t)n_unread);
    activate_result_t act = session_try_activate(job->h, name, fb.buf, fb.used);
    if (act == ACTIVATE_DUPLICATE) {
        char text[96];
        snprintf(text, sizeof text, "%s is already logged in", name);
        reject_login(job, E_ALREADY_LOGGED_IN, text);
        log_event("login rejected for '%s': already logged in", name);
        return;
    }
    if (act == ACTIVATE_STALE)
        return;
    log_event("%s logged in (%d unread)", name, n_unread);

    /* 3. Send every pending message. The session is already ACTIVE, so a
     *    message stored from now on is also delivered live; the client
     *    removes any duplicate by message id. */
    db_messages_t pending = {0};
    if (db_pending_for(name, now, &pending, &err) != DB_OK) {
        log_event("database error: %s", err.msg);
        return;   /* still pending in the DB: delivered at the next login */
    }
    for (int i = 0; i < pending.count; i++) {
        db_message_t *m = &pending.items[i];
        enc_msg(&fb, m->id, m->sender, m->group, m->body, (uint64_t)m->expires_at);
        reply(job, &fb);
    }
    db_messages_free(&pending);
}

/* ======================= direct messages ======================= */

static void handle_send(const job_t *job, reader_t *r, bool timed)
{
    char to[NAME_BUF], text[TEXT_BUF];
    uint32_t seconds = 0;
    rd_str(r, to, sizeof to);
    if (timed)
        seconds = rd_u32(r);
    rd_text(r, text);
    if (!rd_ok(r)) {
        reply_bad_payload(job, r);
        return;
    }
    if (text[0] == '\0') {
        reply_error(job, E_BAD_REQUEST, "message is empty");
        return;
    }
    if (timed && (seconds < 1 || seconds > MAX_TIMED_SECONDS)) {
        reply_error(job, E_TOO_LONG, "seconds must be 1-%d", MAX_TIMED_SECONDS);
        return;
    }
    if (is_group_name(to)) {
        reply_error(job, E_BAD_REQUEST, "use sendgroup to message a group");
        return;
    }
    if (!check_dm_peer(job, to))
        return;

    time_t now = time(NULL);
    time_t expires_at = timed ? now + (time_t)seconds : 0;
    uint64_t id = 0;
    db_err_t err;
    if (db_store_dm(job->username, to, text, now, expires_at, &id, &err) != DB_OK) {
        reply_db_error(job, &err);
        return;
    }

    /* Stored as pending first; now try live delivery (SPEC §5.1). */
    frame_builder_t fb;
    enc_msg(&fb, id, job->username, "", text, (uint64_t)expires_at);
    bool online = push_to_user(to, &fb);

    char ok_text[96];
    if (online)
        snprintf(ok_text, sizeof ok_text, "sent");
    else
        snprintf(ok_text, sizeof ok_text, "stored, %s is offline", to);
    reply_ok(job, id, ok_text);

    if (timed)
        expiry_notify();   /* after the commit: the expiry thread re-reads MIN */

    log_event("%s -> %s #%llu %s%s", job->username, to, (unsigned long long)id,
              online ? "delivered" : "stored (offline)", timed ? " (timed)" : "");
}

static void handle_ack(const job_t *job, reader_t *r)
{
    uint64_t id = rd_u64(r);
    if (!rd_ok(r))
        return;   /* ACK has no reply, not even an error */
    db_err_t err;
    if (db_ack(job->username, id, time(NULL), &err) != DB_OK)
        log_event("database error: %s", err.msg);
}

static void handle_mark_seen(const job_t *job, reader_t *r)
{
    uint64_t id = rd_u64(r);
    if (!rd_ok(r))
        return;
    bool changed = false;
    char sender[NAME_BUF], preview[PREVIEW_MAX + 1];
    db_err_t err;
    if (db_mark_seen(job->username, id, time(NULL), &changed, sender, preview, &err) != DB_OK) {
        log_event("database error: %s", err.msg);
        return;
    }
    if (changed) {
        frame_builder_t fb;
        enc_receipt(&fb, id, job->username, preview);
        push_to_user(sender, &fb);   /* only if the sender is online (F2.1) */
    }
}

/* ======================= history ======================= */

static void send_history(const job_t *job, const db_messages_t *list)
{
    frame_builder_t fb;
    for (int i = 0; i < list->count; i++) {
        const db_message_t *m = &list->items[i];
        const char *to = m->group[0] != '\0' ? m->group : m->recipient;
        uint8_t status = m->group[0] != '\0' ? STATUS_NA : m->status;
        enc_history_item(&fb, m->id, m->sender, to, m->body, status,
                         (uint64_t)m->created_at, (uint64_t)m->expires_at);
        reply(job, &fb);
    }
    enc_history_end(&fb, (uint32_t)list->count);
    reply(job, &fb);
}

static void handle_get_history(const job_t *job, reader_t *r)
{
    char peer[NAME_BUF];
    rd_str(r, peer, sizeof peer);
    if (!rd_ok(r)) {
        reply_bad_payload(job, r);
        return;
    }

    db_messages_t list = {0};
    db_err_t err;
    db_status_t st;
    if (is_group_name(peer)) {
        if (!valid_groupname(peer)) {
            reply_error(job, E_BAD_REQUEST, "invalid group name '%s'", peer);
            return;
        }
        st = db_history_group(peer, job->username, &list, &err);
        if (st != DB_OK) {
            reply_group_status(job, st, peer, &err, "");
            db_messages_free(&list);
            return;
        }
    } else {
        if (!check_dm_peer(job, peer))
            return;
        st = db_history_dm(job->username, peer, time(NULL), &list, &err);
        if (st != DB_OK) {
            reply_db_error(job, &err);
            db_messages_free(&list);
            return;
        }
    }
    send_history(job, &list);
    db_messages_free(&list);
}

static void handle_delete_history(const job_t *job, reader_t *r)
{
    char peer[NAME_BUF];
    rd_str(r, peer, sizeof peer);
    if (!rd_ok(r)) {
        reply_bad_payload(job, r);
        return;
    }

    int count = 0;
    db_err_t err;
    if (is_group_name(peer)) {
        if (!valid_groupname(peer)) {
            reply_error(job, E_BAD_REQUEST, "invalid group name '%s'", peer);
            return;
        }
        char not_creator[96];
        snprintf(not_creator, sizeof not_creator, "only the creator can delete %s history", peer);
        db_status_t st = db_delete_group_history(peer, job->username, &count, &err);
        if (st != DB_OK) {
            reply_group_status(job, st, peer, &err, not_creator);
            return;
        }
    } else {
        if (!check_dm_peer(job, peer))
            return;
        if (db_delete_dm(job->username, peer, time(NULL), &count, &err) != DB_OK) {
            reply_db_error(job, &err);
            return;
        }
    }
    reply_ok(job, (uint64_t)count, "deleted");
    log_event("%s deleted %d message(s) with %s", job->username, count, peer);
}

/* ======================= users ======================= */

static void handle_get_users(const job_t *job)
{
    char names[MAX_CLIENTS][NAME_BUF];
    int n = session_online_list(names, MAX_CLIENTS);
    frame_builder_t fb;
    enc_name_list(&fb, T_USER_LIST, names, n);
    reply(job, &fb);
}

/* ======================= groups ======================= */

/* Reads and checks a group name field. Replies on failure. */
static bool read_group_name(const job_t *job, reader_t *r, char *group)
{
    rd_str(r, group, NAME_BUF);
    if (r->err) {
        reply_bad_payload(job, r);
        return false;
    }
    if (!valid_groupname(group)) {
        reply_error(job, E_BAD_REQUEST, "invalid group name '%s' (use #name)", group);
        return false;
    }
    return true;
}

static void handle_group_create(const job_t *job, reader_t *r)
{
    char group[NAME_BUF];
    if (!read_group_name(job, r, group))
        return;
    uint16_t n = rd_u16(r);
    if (r->err || n > MAX_GROUP_MEMBERS) {
        reply_error(job, E_BAD_REQUEST, "too many members (max %d)", MAX_GROUP_MEMBERS);
        return;
    }
    char members[MAX_GROUP_MEMBERS][NAME_BUF];
    for (int i = 0; i < n; i++)
        rd_str(r, members[i], NAME_BUF);
    if (!rd_ok(r)) {
        reply_bad_payload(job, r);
        return;
    }
    /* Every listed name must exist; otherwise nothing is created (F3.1). */
    for (int i = 0; i < n; i++) {
        if (!valid_username(members[i]) || !users_exists(members[i])) {
            reply_error(job, E_UNKNOWN_USER, "unknown user '%s'", members[i]);
            return;
        }
    }

    db_err_t err;
    db_status_t st = db_group_create(group, job->username, members, n, time(NULL), &err);
    if (st != DB_OK) {
        reply_group_status(job, st, group, &err, "");
        return;
    }
    reply_ok(job, 0, "created");
    log_event("%s created %s", job->username, group);
}

static void handle_group_add(const job_t *job, reader_t *r)
{
    char group[NAME_BUF], user[NAME_BUF];
    if (!read_group_name(job, r, group))
        return;
    rd_str(r, user, sizeof user);
    if (!rd_ok(r)) {
        reply_bad_payload(job, r);
        return;
    }
    if (!valid_username(user) || !users_exists(user)) {
        reply_error(job, E_UNKNOWN_USER, "unknown user '%s'", user);
        return;
    }

    db_err_t err;
    db_status_t st = db_group_add(group, job->username, user, &err);
    if (st == DB_EXISTS) {
        reply_error(job, E_BAD_REQUEST, "%s is already a member of %s", user, group);
        return;
    }
    if (st != DB_OK) {
        reply_group_status(job, st, group, &err, "only the creator can add members");
        return;
    }

    char text[128];
    snprintf(text, sizeof text, "[%s added you to %s]", job->username, group);
    frame_builder_t fb;
    enc_text_frame(&fb, T_GROUP_EVENT, text);
    push_to_user(user, &fb);
    reply_ok(job, 0, "added");
    log_event("%s added %s to %s", job->username, user, group);
}

static void handle_group_send(const job_t *job, reader_t *r)
{
    char group[NAME_BUF], text[TEXT_BUF];
    if (!read_group_name(job, r, group))
        return;
    rd_text(r, text);
    if (!rd_ok(r)) {
        reply_bad_payload(job, r);
        return;
    }
    if (text[0] == '\0') {
        reply_error(job, E_BAD_REQUEST, "message is empty");
        return;
    }

    uint64_t id = 0;
    db_names_t recipients = {0};
    db_err_t err;
    db_status_t st = db_store_group_msg(group, job->username, text, time(NULL), &id,
                                        &recipients, &err);
    if (st != DB_OK) {
        reply_group_status(job, st, group, &err, "");
        db_names_free(&recipients);
        return;
    }

    /* Live fan-out to exactly the members stored with the message (F3.8). */
    frame_builder_t fb;
    enc_msg(&fb, id, job->username, group, text, 0);
    int online = 0;
    for (int i = 0; i < recipients.count; i++)
        if (push_to_user(recipients.names[i], &fb))
            online++;
    reply_ok(job, id, "sent");
    log_event("%s -> %s #%llu (%d of %d members online)", job->username, group,
              (unsigned long long)id, online, recipients.count);
    db_names_free(&recipients);
}

static void handle_group_leave(const job_t *job, reader_t *r)
{
    char group[NAME_BUF];
    if (!read_group_name(job, r, group))
        return;
    if (!rd_ok(r)) {
        reply_bad_payload(job, r);
        return;
    }

    bool dissolved = false;
    db_names_t others = {0};
    db_err_t err;
    db_status_t st = db_group_leave(group, job->username, &dissolved, &others, &err);
    if (st != DB_OK) {
        reply_group_status(job, st, group, &err, "");
        db_names_free(&others);
        return;
    }

    if (dissolved) {
        char text[128];
        snprintf(text, sizeof text, "[%s was dissolved by %s]", group, job->username);
        frame_builder_t fb;
        enc_text_frame(&fb, T_GROUP_EVENT, text);
        for (int i = 0; i < others.count; i++)
            push_to_user(others.names[i], &fb);
    }
    reply_ok(job, 0, dissolved ? "dissolved" : "left");
    log_event("%s left %s%s", job->username, group, dissolved ? " (group dissolved)" : "");
    db_names_free(&others);
}

static void handle_group_list(const job_t *job)
{
    db_names_t groups = {0};
    db_err_t err;
    if (db_user_groups(job->username, &groups, &err) != DB_OK) {
        reply_db_error(job, &err);
        db_names_free(&groups);
        return;
    }
    frame_builder_t fb;
    enc_name_list(&fb, T_GROUP_LIST_RESULT, groups.names, groups.count);
    if (fb.err)
        reply_error(job, E_INTERNAL, "too many groups to list");
    else
        reply(job, &fb);
    db_names_free(&groups);
}

/* ======================= dispatch ======================= */

void handle_job(const job_t *job)
{
    reader_t r;
    reader_init(&r, job->req);
    uint8_t type = job->req->type;
    bool logged_in = job->username[0] != '\0';

    if (!logged_in && type != T_LOGIN) {
        reply_error(job, E_NOT_LOGGED_IN, "please log in first");
        return;
    }
    if (logged_in && type == T_LOGIN) {
        reply_error(job, E_BAD_REQUEST, "already logged in");
        return;
    }

    switch (type) {
    case T_LOGIN:          handle_login(job, &r); break;
    case T_SEND:           handle_send(job, &r, false); break;
    case T_SEND_TIMED:     handle_send(job, &r, true); break;
    case T_GET_HISTORY:    handle_get_history(job, &r); break;
    case T_DELETE_HISTORY: handle_delete_history(job, &r); break;
    case T_GET_USERS:      handle_get_users(job); break;
    case T_GROUP_CREATE:   handle_group_create(job, &r); break;
    case T_GROUP_ADD:      handle_group_add(job, &r); break;
    case T_GROUP_SEND:     handle_group_send(job, &r); break;
    case T_GROUP_LEAVE:    handle_group_leave(job, &r); break;
    case T_GROUP_LIST:     handle_group_list(job); break;
    case T_ACK:            handle_ack(job, &r); break;
    case T_MARK_SEEN:      handle_mark_seen(job, &r); break;
    case T_LOGOUT:         session_request_close(job->h, CLOSE_NOW); break;
    default:               reply_error(job, E_BAD_REQUEST, "unknown request type 0x%02x", type);
    }
}
