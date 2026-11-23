/*
 * db.h — all SQLite access (SPEC §8).
 *
 * There is one SQLite connection, protected by db_mutex. EVERY function below
 * locks db_mutex at the start and unlocks it before returning, and does no
 * other locking and no logging inside (Rev 3.1 errata: never log while holding
 * a lock). On failure a function returns DB_ERROR and puts the SQLite message
 * in *err; the caller logs it after the call and answers ERROR(INTERNAL).
 *
 * Every function follows the same pattern:
 *     lock -> (BEGIN) -> prepare / bind / step -> finalize -> (COMMIT) -> unlock
 *
 * Result lists (db_messages_t, db_names_t) are malloc'd; free them with
 * db_messages_free / db_names_free AFTER the call, i.e. without db_mutex.
 */
#ifndef DB_H
#define DB_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "protocol.h"

typedef enum {
    DB_OK = 0,
    DB_ERROR,        /* SQLite failure; message in err->msg  */
    DB_EXISTS,       /* group name taken / already a member   */
    DB_NO_GROUP,     /* group does not exist                  */
    DB_NOT_MEMBER,   /* caller is not in the group            */
    DB_NOT_CREATOR,  /* creator-only operation                */
} db_status_t;

typedef struct {
    char msg[200];
} db_err_t;

/* One stored message, as returned by the read queries. */
typedef struct {
    uint64_t id;
    char     sender[NAME_BUF];
    char     recipient[NAME_BUF];  /* DMs; "" for group messages  */
    char     group[NAME_BUF];      /* groups; "" for DMs           */
    char     body[TEXT_BUF];
    uint8_t  status;               /* STATUS_* (255 for groups)    */
    int64_t  created_at;
    int64_t  expires_at;           /* 0 = never                    */
} db_message_t;

typedef struct {
    db_message_t *items;
    int           count, cap;
} db_messages_t;

typedef struct {
    char (*names)[NAME_BUF];
    int    count, cap;
} db_names_t;

void db_messages_free(db_messages_t *list);
void db_names_free(db_names_t *list);

/* Opens (creating if needed) the database and applies the schema.
 * path may be ":memory:" for unit tests. */
db_status_t db_open(const char *path, db_err_t *err);
void        db_close(void);

/* ---------- direct messages ---------- */

/* Stores a pending DM. expires_at = 0 means it never expires. *id = new id. */
db_status_t db_store_dm(const char *sender, const char *recipient, const char *body,
                        time_t now, time_t expires_at, uint64_t *id, db_err_t *err);

/* Number of pending, unexpired messages for user (DMs + group rows of
 * groups the user is currently a member of). */
db_status_t db_count_pending(const char *user, time_t now, int *count, db_err_t *err);

/* Pending, unexpired DMs and pending group messages for user, in id order. */
db_status_t db_pending_for(const char *user, time_t now, db_messages_t *out, db_err_t *err);

/* ACK: pending -> delivered, for the DM or the group delivery row of user.
 * Idempotent. */
db_status_t db_ack(const char *user, uint64_t id, time_t now, db_err_t *err);

/* MARK_SEEN (DMs only): -> seen. If a row changed, *changed = true and the
 * sender's name and a body preview are returned for the RECEIPT. Idempotent. */
db_status_t db_mark_seen(const char *user, uint64_t id, time_t now, bool *changed,
                         char *sender_out, char *preview_out, db_err_t *err);

/* All unexpired DMs between me and peer, in id order. */
db_status_t db_history_dm(const char *me, const char *peer, time_t now,
                          db_messages_t *out, db_err_t *err);

/* Deletes the conversation between me and peer for both of them. */
db_status_t db_delete_dm(const char *me, const char *peer, time_t now, int *count,
                         db_err_t *err);

/* ---------- groups ---------- */

/* Creates group with creator + members (members already checked against
 * users.txt). Everyone's join watermark is the current highest message id.
 * DB_EXISTS if the name is taken. One transaction. */
db_status_t db_group_create(const char *group, const char *creator,
                            char (*members)[NAME_BUF], int n, time_t now, db_err_t *err);

/* Adds user to group; only the creator may do it. DB_EXISTS if already in. */
db_status_t db_group_add(const char *group, const char *by, const char *user,
                         db_err_t *err);

/* user leaves group. If user is the creator the group is dissolved (deleted
 * with its history) and *dissolved = true; *others gets every other member so
 * they can be told. One transaction. */
db_status_t db_group_leave(const char *group, const char *user, bool *dissolved,
                           db_names_t *others, db_err_t *err);

/* Stores a group message and one pending delivery row per member except the
 * sender, in ONE transaction; *recipients = exactly those members (F3.8). */
db_status_t db_store_group_msg(const char *group, const char *sender, const char *body,
                               time_t now, uint64_t *id, db_names_t *recipients,
                               db_err_t *err);

/* Group history after the caller's join watermark (members only). */
db_status_t db_history_group(const char *group, const char *me, db_messages_t *out,
                             db_err_t *err);

/* Deletes the group's history for everyone; creator only. */
db_status_t db_delete_group_history(const char *group, const char *me, int *count,
                                    db_err_t *err);

/* Names of the groups user belongs to, sorted. */
db_status_t db_user_groups(const char *user, db_names_t *out, db_err_t *err);

/* ---------- expiry (F1) ---------- */

/* Earliest expires_at of any message, or *next = 0 if none. */
db_status_t db_next_expiry(time_t *next, db_err_t *err);

/* Deletes every message with expires_at <= now, in one transaction, and
 * returns them (id, sender, recipient) so recipients can be notified. */
db_status_t db_delete_expired(time_t now, db_messages_t *out, db_err_t *err);

#endif
