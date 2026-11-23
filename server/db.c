/*
 * db.c — all SQLite access. See db.h for the locking rule.
 *
 * Structure: every public function is a tiny wrapper
 *
 *     lock db_mutex;  status = xxx_locked(...);  unlock db_mutex;  return status;
 *
 * and the real work is in the matching static xxx_locked function, which may
 * assume db_mutex is held. Functions that change several rows use
 * begin() ... finish(status), which COMMITs on success and ROLLBACKs otherwise.
 */
#include "db.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"

/* db_mutex: SQLite is used through ONE connection shared by the 4 workers and
 * the expiry thread; this mutex makes each db_* call run alone. */
static pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;
static sqlite3        *db = NULL;

static const char *SCHEMA =
    "PRAGMA foreign_keys = ON;"
    "PRAGMA journal_mode = WAL;"
    "PRAGMA synchronous = NORMAL;"
    "CREATE TABLE IF NOT EXISTS groups ("
    "    name        TEXT PRIMARY KEY,"
    "    creator     TEXT NOT NULL,"
    "    created_at  INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS messages ("
    "    id           INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    sender       TEXT    NOT NULL,"
    "    recipient    TEXT,"
    "    group_name   TEXT REFERENCES groups(name) ON DELETE CASCADE,"
    "    body         TEXT    NOT NULL,"
    "    created_at   INTEGER NOT NULL,"
    "    state        INTEGER NOT NULL DEFAULT 0,"
    "    delivered_at INTEGER,"
    "    seen_at      INTEGER,"
    "    expires_at   INTEGER,"
    "    CHECK ((recipient IS NULL) <> (group_name IS NULL))"
    ");"
    "CREATE TABLE IF NOT EXISTS group_members ("
    "    group_name      TEXT NOT NULL REFERENCES groups(name) ON DELETE CASCADE,"
    "    username        TEXT NOT NULL,"
    "    joined_after_id INTEGER NOT NULL,"
    "    PRIMARY KEY (group_name, username)"
    ");"
    "CREATE TABLE IF NOT EXISTS group_delivery ("
    "    message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
    "    username   TEXT    NOT NULL,"
    "    state      INTEGER NOT NULL DEFAULT 0,"
    "    PRIMARY KEY (message_id, username)"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_msg_recipient ON messages(recipient, state);"
    "CREATE INDEX IF NOT EXISTS idx_msg_expiry    ON messages(expires_at) WHERE expires_at IS NOT NULL;"
    "CREATE INDEX IF NOT EXISTS idx_msg_group     ON messages(group_name, id);"
    "CREATE INDEX IF NOT EXISTS idx_gd_user       ON group_delivery(username, state);";

/* The columns every message query selects, in the order read_message expects. */
#define MSG_COLUMNS                                                                 \
    "m.id, m.sender, COALESCE(m.recipient, ''), COALESCE(m.group_name, ''), m.body, " \
    "m.state, m.created_at, COALESCE(m.expires_at, 0)"

/* ======================= small helpers (db_mutex held) ======================= */

static db_status_t fail(db_err_t *err, const char *what)
{
    snprintf(err->msg, sizeof err->msg, "%s: %s", what, sqlite3_errmsg(db));
    return DB_ERROR;
}

static sqlite3_stmt *prepare(const char *sql, db_err_t *err)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fail(err, "prepare");
        return NULL;
    }
    return st;
}

static void bind_text(sqlite3_stmt *st, int i, const char *s)
{
    sqlite3_bind_text(st, i, s, -1, SQLITE_TRANSIENT);
}

/* Runs a statement that returns no rows, then finalizes it. */
static db_status_t step_done(sqlite3_stmt *st, db_err_t *err, const char *what)
{
    int rc = sqlite3_step(st);
    db_status_t status = DB_OK;
    if (rc != SQLITE_DONE)
        status = fail(err, what);
    sqlite3_finalize(st);
    return status;
}

static db_status_t exec_sql(const char *sql, db_err_t *err)
{
    char *msg = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &msg) != SQLITE_OK) {
        snprintf(err->msg, sizeof err->msg, "%s: %s", sql, msg ? msg : "?");
        sqlite3_free(msg);
        return DB_ERROR;
    }
    return DB_OK;
}

/* BEGIN IMMEDIATE takes the write lock at the start, so the checks done
 * inside the transaction stay true until COMMIT. */
static db_status_t begin(db_err_t *err) { return exec_sql("BEGIN IMMEDIATE", err); }

/* Ends a transaction: COMMIT if status is DB_OK, otherwise ROLLBACK.
 * Returns the final status. */
static db_status_t finish(db_status_t status, db_err_t *err)
{
    if (status == DB_OK) {
        status = exec_sql("COMMIT", err);
        if (status == DB_OK)
            return DB_OK;
    }
    db_err_t ignored;
    exec_sql("ROLLBACK", &ignored);
    return status;
}

static void copy_column(char *out, size_t size, sqlite3_stmt *st, int col)
{
    const unsigned char *s = sqlite3_column_text(st, col);
    snprintf(out, size, "%s", s ? (const char *)s : "");
}

static void messages_push(db_messages_t *list, const db_message_t *m)
{
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 16;
        list->items = realloc(list->items, (size_t)list->cap * sizeof(db_message_t));
    }
    list->items[list->count++] = *m;
}

static void names_push(db_names_t *list, const char *name)
{
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 8;
        list->names = realloc(list->names, (size_t)list->cap * NAME_BUF);
    }
    snprintf(list->names[list->count++], NAME_BUF, "%s", name);
}

/* Steps through a query that selects MSG_COLUMNS and appends every row. */
static db_status_t collect_messages(sqlite3_stmt *st, db_messages_t *out, db_err_t *err)
{
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        db_message_t m;
        m.id = (uint64_t)sqlite3_column_int64(st, 0);
        copy_column(m.sender, sizeof m.sender, st, 1);
        copy_column(m.recipient, sizeof m.recipient, st, 2);
        copy_column(m.group, sizeof m.group, st, 3);
        copy_column(m.body, sizeof m.body, st, 4);
        m.status = (uint8_t)sqlite3_column_int(st, 5);
        m.created_at = sqlite3_column_int64(st, 6);
        m.expires_at = sqlite3_column_int64(st, 7);
        messages_push(out, &m);
    }
    db_status_t status = rc == SQLITE_DONE ? DB_OK : fail(err, "query");
    sqlite3_finalize(st);
    return status;
}

/* Steps through a query whose first column is a name and appends every row. */
static db_status_t collect_names(sqlite3_stmt *st, db_names_t *out, db_err_t *err)
{
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW)
        names_push(out, (const char *)sqlite3_column_text(st, 0));
    db_status_t status = rc == SQLITE_DONE ? DB_OK : fail(err, "query");
    sqlite3_finalize(st);
    return status;
}

/* Runs a query returning one integer (0 if the result is NULL). */
static db_status_t query_int(sqlite3_stmt *st, int64_t *value, db_err_t *err)
{
    int rc = sqlite3_step(st);
    db_status_t status = DB_OK;
    *value = 0;
    if (rc == SQLITE_ROW)
        *value = sqlite3_column_int64(st, 0);
    else if (rc != SQLITE_DONE)
        status = fail(err, "query");
    sqlite3_finalize(st);
    return status;
}

/* Highest message id so far: the join watermark for new members. */
static db_status_t current_max_id(int64_t *max_id, db_err_t *err)
{
    sqlite3_stmt *st = prepare("SELECT COALESCE(MAX(id), 0) FROM messages", err);
    if (st == NULL)
        return DB_ERROR;
    return query_int(st, max_id, err);
}

/* Looks up a group's creator. DB_NO_GROUP if the group does not exist. */
static db_status_t group_creator(const char *group, char *creator, db_err_t *err)
{
    sqlite3_stmt *st = prepare("SELECT creator FROM groups WHERE name = ?", err);
    if (st == NULL)
        return DB_ERROR;
    bind_text(st, 1, group);
    int rc = sqlite3_step(st);
    db_status_t status = DB_OK;
    if (rc == SQLITE_ROW)
        copy_column(creator, NAME_BUF, st, 0);
    else if (rc == SQLITE_DONE)
        status = DB_NO_GROUP;
    else
        status = fail(err, "group lookup");
    sqlite3_finalize(st);
    return status;
}

/* Looks up user's join watermark in group.
 * DB_NO_GROUP / DB_NOT_MEMBER if the group or the membership is missing. */
static db_status_t member_watermark(const char *group, const char *user, int64_t *mark,
                                    db_err_t *err)
{
    char creator[NAME_BUF];
    db_status_t status = group_creator(group, creator, err);
    if (status != DB_OK)
        return status;

    sqlite3_stmt *st = prepare(
        "SELECT joined_after_id FROM group_members WHERE group_name = ? AND username = ?",
        err);
    if (st == NULL)
        return DB_ERROR;
    bind_text(st, 1, group);
    bind_text(st, 2, user);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        *mark = sqlite3_column_int64(st, 0);
    else if (rc == SQLITE_DONE)
        status = DB_NOT_MEMBER;
    else
        status = fail(err, "member lookup");
    sqlite3_finalize(st);
    return status;
}

/* Inserts one membership row. DB_EXISTS if the user is already a member. */
static db_status_t insert_member(const char *group, const char *user, int64_t mark,
                                 db_err_t *err)
{
    sqlite3_stmt *st = prepare(
        "INSERT INTO group_members(group_name, username, joined_after_id) VALUES (?, ?, ?)",
        err);
    if (st == NULL)
        return DB_ERROR;
    bind_text(st, 1, group);
    bind_text(st, 2, user);
    sqlite3_bind_int64(st, 3, mark);
    int rc = sqlite3_step(st);
    db_status_t status = DB_OK;
    if ((rc & 0xff) == SQLITE_CONSTRAINT)
        status = DB_EXISTS;
    else if (rc != SQLITE_DONE)
        status = fail(err, "insert member");
    sqlite3_finalize(st);
    return status;
}

/* ======================= lists ======================= */

void db_messages_free(db_messages_t *list)
{
    free(list->items);
    list->items = NULL;
    list->count = list->cap = 0;
}

void db_names_free(db_names_t *list)
{
    free(list->names);
    list->names = NULL;
    list->count = list->cap = 0;
}

/* ======================= open / close ======================= */

db_status_t db_open(const char *path, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = DB_OK;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        status = fail(err, "open");
        sqlite3_close(db);
        db = NULL;
    } else {
        sqlite3_busy_timeout(db, 2000);
        status = exec_sql(SCHEMA, err);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

void db_close(void)
{
    pthread_mutex_lock(&db_mutex);
    sqlite3_close(db);
    db = NULL;
    pthread_mutex_unlock(&db_mutex);
}

/* ======================= direct messages ======================= */

static db_status_t store_dm_locked(const char *sender, const char *recipient,
                                   const char *body, time_t now, time_t expires_at,
                                   uint64_t *id, db_err_t *err)
{
#ifdef CHAT_TEST_HOOKS
    /* Test hook for X8: pretend the database failed. */
    if (getenv("CHAT_FAIL_STORE_DM") != NULL) {
        snprintf(err->msg, sizeof err->msg, "simulated failure (CHAT_FAIL_STORE_DM)");
        return DB_ERROR;
    }
#endif
    sqlite3_stmt *st = prepare(
        "INSERT INTO messages(sender, recipient, body, created_at, state, expires_at) "
        "VALUES (?, ?, ?, ?, 0, ?)",
        err);
    if (st == NULL)
        return DB_ERROR;
    bind_text(st, 1, sender);
    bind_text(st, 2, recipient);
    bind_text(st, 3, body);
    sqlite3_bind_int64(st, 4, now);
    if (expires_at > 0)
        sqlite3_bind_int64(st, 5, expires_at);
    else
        sqlite3_bind_null(st, 5);
    db_status_t status = step_done(st, err, "store dm");
    if (status == DB_OK)
        *id = (uint64_t)sqlite3_last_insert_rowid(db);
    return status;
}

db_status_t db_store_dm(const char *sender, const char *recipient, const char *body,
                        time_t now, time_t expires_at, uint64_t *id, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = store_dm_locked(sender, recipient, body, now, expires_at, id, err);
    pthread_mutex_unlock(&db_mutex);
    return status;
}

/* Pending DMs (unexpired) + pending group rows, but only for groups the user
 * is CURRENTLY in and only after their join watermark (Rev 3.1 errata). */
#define PENDING_SQL                                                                   \
    "SELECT " MSG_COLUMNS " FROM messages m "                                         \
    " WHERE m.recipient = ?1 AND m.state = 0 "                                        \
    "   AND (m.expires_at IS NULL OR m.expires_at > ?2) "                             \
    "UNION ALL "                                                                      \
    "SELECT " MSG_COLUMNS " FROM group_delivery gd "                                  \
    "  JOIN messages m       ON m.id = gd.message_id "                                \
    "  JOIN group_members gm ON gm.group_name = m.group_name AND gm.username = gd.username " \
    " WHERE gd.username = ?1 AND gd.state = 0 AND m.id > gm.joined_after_id "

db_status_t db_count_pending(const char *user, time_t now, int *count, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = DB_ERROR;
    sqlite3_stmt *st = prepare("SELECT COUNT(*) FROM (" PENDING_SQL ")", err);
    if (st != NULL) {
        bind_text(st, 1, user);
        sqlite3_bind_int64(st, 2, now);
        int64_t n;
        status = query_int(st, &n, err);
        *count = (int)n;
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

db_status_t db_pending_for(const char *user, time_t now, uint64_t after_id, int limit,
                           db_messages_t *out, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = DB_ERROR;
    sqlite3_stmt *st = prepare("SELECT * FROM (" PENDING_SQL ") WHERE id > ?3 ORDER BY id LIMIT ?4",
                               err);
    if (st != NULL) {
        bind_text(st, 1, user);
        sqlite3_bind_int64(st, 2, now);
        sqlite3_bind_int64(st, 3, (int64_t)after_id);
        sqlite3_bind_int(st, 4, limit);
        status = collect_messages(st, out, err);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

static db_status_t ack_locked(const char *user, uint64_t id, time_t now, db_err_t *err)
{
    /* Choose the path by whether the message belongs to a group (SPEC §5.3). */
    sqlite3_stmt *st = prepare("SELECT group_name IS NOT NULL FROM messages WHERE id = ?", err);
    if (st == NULL)
        return DB_ERROR;
    sqlite3_bind_int64(st, 1, (int64_t)id);
    int64_t is_group = 0;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        is_group = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE)
        return DB_OK;                 /* message gone (deleted/expired): nothing to do */
    if (rc != SQLITE_ROW)
        return fail(err, "ack lookup");

    if (is_group) {
        st = prepare("UPDATE group_delivery SET state = 1 "
                     "WHERE message_id = ? AND username = ? AND state = 0", err);
        if (st == NULL)
            return DB_ERROR;
        sqlite3_bind_int64(st, 1, (int64_t)id);
        bind_text(st, 2, user);
    } else {
        st = prepare("UPDATE messages SET state = 1, delivered_at = ? "
                     "WHERE id = ? AND recipient = ? AND state = 0", err);
        if (st == NULL)
            return DB_ERROR;
        sqlite3_bind_int64(st, 1, now);
        sqlite3_bind_int64(st, 2, (int64_t)id);
        bind_text(st, 3, user);
    }
    return step_done(st, err, "ack");
}

db_status_t db_ack(const char *user, uint64_t id, time_t now, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = ack_locked(user, id, now, err);
    pthread_mutex_unlock(&db_mutex);
    return status;
}

static db_status_t mark_seen_locked(const char *user, uint64_t id, time_t now, bool *changed,
                                    char *sender_out, char *preview_out, db_err_t *err)
{
    *changed = false;
    sqlite3_stmt *st = prepare(
        "UPDATE messages SET state = 2, seen_at = ?1, delivered_at = COALESCE(delivered_at, ?1) "
        "WHERE id = ?2 AND recipient = ?3 AND state < 2 AND group_name IS NULL",
        err);
    if (st == NULL)
        return DB_ERROR;
    sqlite3_bind_int64(st, 1, now);
    sqlite3_bind_int64(st, 2, (int64_t)id);
    bind_text(st, 3, user);
    db_status_t status = step_done(st, err, "mark seen");
    if (status != DB_OK || sqlite3_changes(db) == 0)
        return status;

    st = prepare("SELECT sender, body FROM messages WHERE id = ?", err);
    if (st == NULL)
        return DB_ERROR;
    sqlite3_bind_int64(st, 1, (int64_t)id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        char body[TEXT_BUF];
        copy_column(sender_out, NAME_BUF, st, 0);
        copy_column(body, sizeof body, st, 1);
        make_preview(body, preview_out);
        *changed = true;
    }
    sqlite3_finalize(st);
    return DB_OK;
}

db_status_t db_mark_seen(const char *user, uint64_t id, time_t now, bool *changed,
                         char *sender_out, char *preview_out, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = mark_seen_locked(user, id, now, changed, sender_out, preview_out, err);
    pthread_mutex_unlock(&db_mutex);
    return status;
}

/* The DM conversation between ?1 and ?2, unexpired at time ?3. */
#define CONVERSATION_WHERE                                                         \
    " WHERE ((m.sender = ?1 AND m.recipient = ?2) OR (m.sender = ?2 AND m.recipient = ?1)) " \
    "   AND (m.expires_at IS NULL OR m.expires_at > ?3) "

db_status_t db_history_dm(const char *me, const char *peer, time_t now,
                          db_messages_t *out, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = DB_ERROR;
    sqlite3_stmt *st = prepare("SELECT " MSG_COLUMNS " FROM messages m " CONVERSATION_WHERE
                               " ORDER BY m.id", err);
    if (st != NULL) {
        bind_text(st, 1, me);
        bind_text(st, 2, peer);
        sqlite3_bind_int64(st, 3, now);
        status = collect_messages(st, out, err);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

db_status_t db_delete_dm(const char *me, const char *peer, time_t now, int *count,
                         db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = DB_ERROR;
    *count = 0;
    sqlite3_stmt *st = prepare("DELETE FROM messages AS m " CONVERSATION_WHERE, err);
    if (st != NULL) {
        bind_text(st, 1, me);
        bind_text(st, 2, peer);
        sqlite3_bind_int64(st, 3, now);
        status = step_done(st, err, "delete dm");
        if (status == DB_OK)
            *count = sqlite3_changes(db);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

/* ======================= groups ======================= */

static db_status_t group_create_locked(const char *group, const char *creator,
                                       char (*members)[NAME_BUF], int n, time_t now,
                                       db_err_t *err)
{
    db_status_t status = begin(err);
    if (status != DB_OK)
        return status;

    sqlite3_stmt *st = prepare("INSERT INTO groups(name, creator, created_at) VALUES (?, ?, ?)",
                               err);
    if (st == NULL)
        return finish(DB_ERROR, err);
    bind_text(st, 1, group);
    bind_text(st, 2, creator);
    sqlite3_bind_int64(st, 3, now);
    int rc = sqlite3_step(st);
    if ((rc & 0xff) == SQLITE_CONSTRAINT)   /* primary key: name already taken */
        status = DB_EXISTS;
    else if (rc != SQLITE_DONE)
        status = fail(err, "create group");
    sqlite3_finalize(st);

    int64_t mark = 0;
    if (status == DB_OK)
        status = current_max_id(&mark, err);
    if (status == DB_OK)
        status = insert_member(group, creator, mark, err);
    for (int i = 0; i < n && status == DB_OK; i++) {
        status = insert_member(group, members[i], mark, err);
        if (status == DB_EXISTS)       /* a name listed twice: ignore */
            status = DB_OK;
    }
    return finish(status, err);
}

db_status_t db_group_create(const char *group, const char *creator,
                            char (*members)[NAME_BUF], int n, time_t now, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = group_create_locked(group, creator, members, n, now, err);
    pthread_mutex_unlock(&db_mutex);
    return status;
}

static db_status_t group_add_locked(const char *group, const char *by, const char *user,
                                    db_err_t *err)
{
    db_status_t status = begin(err);
    if (status != DB_OK)
        return status;

    char creator[NAME_BUF];
    status = group_creator(group, creator, err);
    if (status == DB_OK && strcmp(creator, by) != 0)
        status = DB_NOT_CREATOR;
    int64_t mark = 0;
    if (status == DB_OK)
        status = current_max_id(&mark, err);
    if (status == DB_OK)
        status = insert_member(group, user, mark, err);
    return finish(status, err);
}

db_status_t db_group_add(const char *group, const char *by, const char *user, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = group_add_locked(group, by, user, err);
    pthread_mutex_unlock(&db_mutex);
    return status;
}

static db_status_t group_leave_locked(const char *group, const char *user, bool *dissolved,
                                      db_names_t *others, db_err_t *err)
{
    *dissolved = false;
    db_status_t status = begin(err);
    if (status != DB_OK)
        return status;

    char creator[NAME_BUF];
    int64_t mark;
    status = member_watermark(group, user, &mark, err);   /* NO_GROUP / NOT_MEMBER */
    if (status == DB_OK)
        status = group_creator(group, creator, err);
    if (status != DB_OK)
        return finish(status, err);

    sqlite3_stmt *st;
    if (strcmp(creator, user) == 0) {
        /* Creator leaves: tell everyone else, then delete the group. The
         * foreign keys cascade to members, messages and delivery rows. */
        st = prepare("SELECT username FROM group_members "
                     "WHERE group_name = ? AND username <> ? ORDER BY username", err);
        if (st == NULL)
            return finish(DB_ERROR, err);
        bind_text(st, 1, group);
        bind_text(st, 2, user);
        status = collect_names(st, others, err);
        if (status == DB_OK) {
            st = prepare("DELETE FROM groups WHERE name = ?", err);
            if (st == NULL)
                return finish(DB_ERROR, err);
            bind_text(st, 1, group);
            status = step_done(st, err, "dissolve group");
        }
        *dissolved = (status == DB_OK);
    } else {
        st = prepare("DELETE FROM group_members WHERE group_name = ? AND username = ?", err);
        if (st == NULL)
            return finish(DB_ERROR, err);
        bind_text(st, 1, group);
        bind_text(st, 2, user);
        status = step_done(st, err, "leave group");
        if (status == DB_OK) {
            /* Tidy-up: their pending rows for this group are never needed again.
             * (Correctness does not depend on it: the pending query also checks
             * current membership.) */
            st = prepare("DELETE FROM group_delivery WHERE username = ? AND state = 0 "
                         "AND message_id IN (SELECT id FROM messages WHERE group_name = ?)",
                         err);
            if (st == NULL)
                return finish(DB_ERROR, err);
            bind_text(st, 1, user);
            bind_text(st, 2, group);
            status = step_done(st, err, "leave group cleanup");
        }
    }
    return finish(status, err);
}

db_status_t db_group_leave(const char *group, const char *user, bool *dissolved,
                           db_names_t *others, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = group_leave_locked(group, user, dissolved, others, err);
    pthread_mutex_unlock(&db_mutex);
    return status;
}

static db_status_t store_group_msg_locked(const char *group, const char *sender,
                                          const char *body, time_t now, uint64_t *id,
                                          db_names_t *recipients, db_err_t *err)
{
    db_status_t status = begin(err);
    if (status != DB_OK)
        return status;

    int64_t mark;
    status = member_watermark(group, sender, &mark, err);   /* NO_GROUP / NOT_MEMBER */
    if (status != DB_OK)
        return finish(status, err);

    /* state = 255: not used for group messages; delivery is per member in
     * group_delivery (Rev 3.1 errata). */
    sqlite3_stmt *st = prepare(
        "INSERT INTO messages(sender, group_name, body, created_at, state) "
        "VALUES (?, ?, ?, ?, 255)", err);
    if (st == NULL)
        return finish(DB_ERROR, err);
    bind_text(st, 1, sender);
    bind_text(st, 2, group);
    bind_text(st, 3, body);
    sqlite3_bind_int64(st, 4, now);
    status = step_done(st, err, "store group message");
    if (status != DB_OK)
        return finish(status, err);
    *id = (uint64_t)sqlite3_last_insert_rowid(db);

    /* One pending delivery row per member except the sender... */
    st = prepare("INSERT INTO group_delivery(message_id, username) "
                 "SELECT ?, username FROM group_members WHERE group_name = ? AND username <> ?",
                 err);
    if (st == NULL)
        return finish(DB_ERROR, err);
    sqlite3_bind_int64(st, 1, (int64_t)*id);
    bind_text(st, 2, group);
    bind_text(st, 3, sender);
    status = step_done(st, err, "group fan-out");

    /* ...and the same member list, read inside the same transaction, so the
     * live fan-out matches the stored rows exactly (F3.8). */
    if (status == DB_OK) {
        st = prepare("SELECT username FROM group_delivery WHERE message_id = ? ORDER BY username",
                     err);
        if (st == NULL)
            return finish(DB_ERROR, err);
        sqlite3_bind_int64(st, 1, (int64_t)*id);
        status = collect_names(st, recipients, err);
    }
    return finish(status, err);
}

db_status_t db_store_group_msg(const char *group, const char *sender, const char *body,
                               time_t now, uint64_t *id, db_names_t *recipients,
                               db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = store_group_msg_locked(group, sender, body, now, id, recipients, err);
    pthread_mutex_unlock(&db_mutex);
    return status;
}

static db_status_t history_group_locked(const char *group, const char *me,
                                        db_messages_t *out, db_err_t *err)
{
    int64_t mark;
    db_status_t status = member_watermark(group, me, &mark, err);
    if (status != DB_OK)
        return status;
    sqlite3_stmt *st = prepare("SELECT " MSG_COLUMNS " FROM messages m "
                               "WHERE m.group_name = ? AND m.id > ? ORDER BY m.id", err);
    if (st == NULL)
        return DB_ERROR;
    bind_text(st, 1, group);
    sqlite3_bind_int64(st, 2, mark);
    return collect_messages(st, out, err);
}

db_status_t db_history_group(const char *group, const char *me, db_messages_t *out,
                             db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = history_group_locked(group, me, out, err);
    pthread_mutex_unlock(&db_mutex);
    return status;
}

static db_status_t delete_group_history_locked(const char *group, const char *me, int *count,
                                               db_err_t *err)
{
    *count = 0;
    db_status_t status = begin(err);
    if (status != DB_OK)
        return status;

    char creator[NAME_BUF];
    status = group_creator(group, creator, err);
    if (status == DB_OK && strcmp(creator, me) != 0)
        status = DB_NOT_CREATOR;
    if (status == DB_OK) {
        /* group_delivery rows go too, through ON DELETE CASCADE. */
        sqlite3_stmt *st = prepare("DELETE FROM messages WHERE group_name = ?", err);
        if (st == NULL)
            return finish(DB_ERROR, err);
        bind_text(st, 1, group);
        status = step_done(st, err, "delete group history");
        if (status == DB_OK)
            *count = sqlite3_changes(db);
    }
    return finish(status, err);
}

db_status_t db_delete_group_history(const char *group, const char *me, int *count,
                                    db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = delete_group_history_locked(group, me, count, err);
    pthread_mutex_unlock(&db_mutex);
    return status;
}

db_status_t db_user_groups(const char *user, db_names_t *out, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = DB_ERROR;
    sqlite3_stmt *st = prepare(
        "SELECT group_name FROM group_members WHERE username = ? ORDER BY group_name", err);
    if (st != NULL) {
        bind_text(st, 1, user);
        status = collect_names(st, out, err);
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

/* ======================= expiry ======================= */

db_status_t db_next_expiry(time_t *next, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = DB_ERROR;
    *next = 0;
    sqlite3_stmt *st = prepare("SELECT MIN(expires_at) FROM messages WHERE expires_at IS NOT NULL",
                               err);
    if (st != NULL) {
        int64_t value;
        status = query_int(st, &value, err);
        *next = (time_t)value;
    }
    pthread_mutex_unlock(&db_mutex);
    return status;
}

static db_status_t delete_expired_locked(time_t now, db_messages_t *out, db_err_t *err)
{
    db_status_t status = begin(err);
    if (status != DB_OK)
        return status;

    sqlite3_stmt *st = prepare("SELECT " MSG_COLUMNS " FROM messages m "
                               "WHERE m.expires_at IS NOT NULL AND m.expires_at <= ? "
                               "ORDER BY m.id", err);
    if (st == NULL)
        return finish(DB_ERROR, err);
    sqlite3_bind_int64(st, 1, now);
    status = collect_messages(st, out, err);

    if (status == DB_OK && out->count > 0) {
        st = prepare("DELETE FROM messages WHERE expires_at IS NOT NULL AND expires_at <= ?",
                     err);
        if (st == NULL)
            return finish(DB_ERROR, err);
        sqlite3_bind_int64(st, 1, now);
        status = step_done(st, err, "delete expired");
    }
    return finish(status, err);
}

db_status_t db_delete_expired(time_t now, db_messages_t *out, db_err_t *err)
{
    pthread_mutex_lock(&db_mutex);
    db_status_t status = delete_expired_locked(now, out, err);
    pthread_mutex_unlock(&db_mutex);
    return status;
}
