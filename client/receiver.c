/*
 * receiver.c — receives frames from the server. See receiver.h.
 */
#include "receiver.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "net.h"
#include "ui.h"

#define SEEN_IDS 1024   /* size of the duplicate filter (Rev 3.1 errata) */

/* The command the input thread is waiting on (a copy, under ctx_mutex). */
typedef struct {
    bool  active;
    cmd_t cmd;
} pending_t;

/* ctx_mutex: protects pending, stopping and server_closed, which both client
 * threads read and write. */
static pthread_mutex_t ctx_mutex = PTHREAD_MUTEX_INITIALIZER;
static pending_t       pending;
static bool            stopping = false;
static bool            server_closed = false;

/* send_mutex: both threads send on the socket (commands; ACK / MARK_SEEN), and
 * two interleaved send() calls could mix the bytes of two frames. */
static pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             server_fd = -1;

static int       notify_fd = -1;
static pthread_t receiver_thread;
static char      my_name[NAME_BUF];

/* ---- receiver-thread-only state (no lock needed) ---- */

/* Duplicate filter: the most recent SEEN_IDS message ids shown in this
 * session, in a circular buffer searched linearly. Never saved to disk. */
static uint64_t seen_ids[SEEN_IDS];
static int      seen_count, seen_next;

/* HISTORY_ITEMs are collected until HISTORY_END so the header can show the
 * count. */
typedef struct {
    uint64_t id;
    char     from[NAME_BUF], to[NAME_BUF], text[TEXT_BUF];
    uint8_t  status;
    uint64_t created_at, expires_at;
} history_item_t;
static history_item_t *history;
static int             history_count, history_cap;

/* ======================= shared helpers ======================= */

void receiver_init(int fd) { notify_fd = fd; }

static void notify(char byte)
{
    ssize_t ignored = write(notify_fd, &byte, 1);
    (void)ignored;
}

int client_send(const frame_builder_t *fb)
{
    pthread_mutex_lock(&send_mutex);
    int rc = send_frame(server_fd, fb);
    pthread_mutex_unlock(&send_mutex);
    return rc;
}

void pending_set(const cmd_t *cmd)
{
    pthread_mutex_lock(&ctx_mutex);
    pending.active = true;
    pending.cmd = *cmd;
    pthread_mutex_unlock(&ctx_mutex);
}

void pending_clear(void)
{
    pthread_mutex_lock(&ctx_mutex);
    pending.active = false;
    pthread_mutex_unlock(&ctx_mutex);
}

/* Takes the waiting command (copy) and clears it. False if none. */
static bool pending_take(cmd_t *out)
{
    pthread_mutex_lock(&ctx_mutex);
    bool active = pending.active;
    if (active)
        *out = pending.cmd;
    pending.active = false;
    pthread_mutex_unlock(&ctx_mutex);
    return active;
}

void receiver_stopping(void)
{
    pthread_mutex_lock(&ctx_mutex);
    stopping = true;
    pthread_mutex_unlock(&ctx_mutex);
}

bool receiver_server_closed(void)
{
    pthread_mutex_lock(&ctx_mutex);
    bool closed = server_closed;
    pthread_mutex_unlock(&ctx_mutex);
    return closed;
}

/* ======================= duplicate filter ======================= */

static bool seen_before(uint64_t id)
{
    for (int i = 0; i < seen_count; i++)
        if (seen_ids[i] == id)
            return true;
    return false;
}

static void remember_seen(uint64_t id)
{
    seen_ids[seen_next] = id;                /* overwrite the oldest when full */
    seen_next = (seen_next + 1) % SEEN_IDS;
    if (seen_count < SEEN_IDS)
        seen_count++;
}

/* ======================= pushes ======================= */

static void send_id_frame(uint8_t type, uint64_t id)
{
    frame_builder_t fb;
    fb_begin(&fb, type);
    fb_u64(&fb, id);
    fb_finish(&fb);
    client_send(&fb);
}

/* MSG, always in this order (SPEC §5 step 2): validate, skip if duplicate or
 * expired, display, ACK, and for a DM also MARK_SEEN. */
static void on_msg(reader_t *r)
{
    char from[NAME_BUF], group[NAME_BUF], text[TEXT_BUF];
    uint64_t id = rd_u64(r);
    rd_str(r, from, sizeof from);
    rd_str(r, group, sizeof group);
    rd_str(r, text, sizeof text);
    uint64_t expires_at = rd_u64(r);
    if (!rd_ok(r))
        return;                                    /* 1. invalid frame: ignore */

    time_t now = time(NULL);
    bool expired = expires_at != 0 && (time_t)expires_at <= now;
    if (!seen_before(id) && !expired) {            /* 2. skip duplicates / expired */
        remember_seen(id);                         /* 3. display */
        if (group[0] != '\0')
            ui_print("Message from %s in %s: %s", from, group, text);
        else if (expires_at != 0)
            ui_print("Message from %s (disappears in %lds): %s", from,
                     (long)((time_t)expires_at - now), text);
        else
            ui_print("Message from %s: %s", from, text);
    }
    send_id_frame(T_ACK, id);                      /* 4. ACK (duplicates too) */
    if (group[0] == '\0')
        send_id_frame(T_MARK_SEEN, id);            /* 5. DMs: seen */
}

static void on_receipt(reader_t *r)
{
    char reader_name[NAME_BUF], preview[PREVIEW_MAX + 1];
    uint64_t id = rd_u64(r);
    rd_str(r, reader_name, sizeof reader_name);
    rd_str(r, preview, sizeof preview);
    if (rd_ok(r))
        ui_print("[seen] %s saw your message #%llu: \"%s\"", reader_name,
                 (unsigned long long)id, preview);
}

static void on_expired(reader_t *r)
{
    char from[NAME_BUF];
    rd_u64(r);
    rd_str(r, from, sizeof from);
    if (rd_ok(r))
        ui_print("[a message from %s has disappeared]", from);
}

/* ======================= replies ======================= */

static const char *status_word(uint8_t status)
{
    switch (status) {
    case STATUS_PENDING:   return "sent";
    case STATUS_DELIVERED: return "delivered";
    case STATUS_SEEN:      return "seen";
    default:               return NULL;
    }
}

static void print_history(const cmd_t *cmd)
{
    bool group = is_group_name(cmd->name);
    if (history_count == 0) {
        ui_print("No messages %s %s.", group ? "in" : "with", cmd->name);
        return;
    }
    if (group)
        ui_print("--- Messages in %s (%d) ---", cmd->name, history_count);
    else
        ui_print("--- Conversation with %s (%d) ---", cmd->name, history_count);

    time_t now = time(NULL);
    for (int i = 0; i < history_count; i++) {
        history_item_t *h = &history[i];
        char stamp[8];
        time_t created = (time_t)h->created_at;
        struct tm tm;
        localtime_r(&created, &tm);
        strftime(stamp, sizeof stamp, "%H:%M", &tm);

        char line[1200];
        snprintf(line, sizeof line, "[#%llu %s] %s -> %s: %s", (unsigned long long)h->id,
                 stamp, h->from, h->to, h->text);

        /* Status only on my own DMs (F2.2); remaining time on timed ones. */
        char note[64] = "";
        const char *status = NULL;
        if (!group && strcmp(h->from, my_name) == 0)
            status = status_word(h->status);
        long left = h->expires_at ? (long)((time_t)h->expires_at - now) : 0;
        if (status && left > 0)
            snprintf(note, sizeof note, "(%s, disappears in %lds)", status, left);
        else if (status)
            snprintf(note, sizeof note, "(%s)", status);
        else if (left > 0)
            snprintf(note, sizeof note, "(disappears in %lds)", left);

        if (note[0] != '\0')
            ui_print("%-44s %s", line, note);
        else
            ui_print("%s", line);
    }
    ui_print("--- end ---");
}

static void on_history_item(reader_t *r)
{
    history_item_t h;
    h.id = rd_u64(r);
    rd_str(r, h.from, sizeof h.from);
    rd_str(r, h.to, sizeof h.to);
    rd_str(r, h.text, sizeof h.text);
    h.status = rd_u8(r);
    h.created_at = rd_u64(r);
    h.expires_at = rd_u64(r);
    if (!rd_ok(r))
        return;
    if (history_count == history_cap) {
        history_cap = history_cap ? history_cap * 2 : 32;
        history = realloc(history, (size_t)history_cap * sizeof *history);
    }
    history[history_count++] = h;
}

/* Prints "a, b, c" from a list of names. */
static void join_names(char *out, size_t size, char (*names)[NAME_BUF], int n)
{
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i > 0)
            strncat(out, ", ", size - strlen(out) - 1);
        strncat(out, names[i], size - strlen(out) - 1);
    }
}

/* Reads a USER_LIST / GROUP_LIST_RESULT payload. Returns the count, -1 if bad. */
static int read_name_list(reader_t *r, char (*names)[NAME_BUF], int max)
{
    int n = rd_u16(r);
    if (n > max)
        return -1;
    for (int i = 0; i < n; i++)
        rd_str(r, names[i], NAME_BUF);
    return rd_ok(r) ? n : -1;
}

/* Formats the OK reply for the command that was waiting. */
static void print_ok(const cmd_t *cmd, uint64_t ref, const char *text)
{
    bool offline = strncmp(text, "stored", 6) == 0;
    unsigned long long id = (unsigned long long)ref;
    switch (cmd->type) {
    case CMD_SEND:
        if (offline)
            ui_print("Stored for %s (offline) (#%llu).", cmd->name, id);
        else
            ui_print("Sent to %s (#%llu).", cmd->name, id);
        break;
    case CMD_SEND_TIMED:
        if (offline)
            ui_print("Stored for %s (offline) (#%llu), disappears in %us.", cmd->name, id,
                     cmd->seconds);
        else
            ui_print("Sent to %s (#%llu), disappears in %us.", cmd->name, id, cmd->seconds);
        break;
    case CMD_DELETE_HISTORY:
        ui_print("Deleted %llu message(s) %s %s.", id, is_group_name(cmd->name) ? "in" : "with",
                 cmd->name);
        break;
    case CMD_GROUP_CREATE: {
        /* "Created #g with alice, bob, carol." — me first, no repeats. */
        char names[CMD_MAX_MEMBERS + 1][NAME_BUF];
        int n = 0;
        strcpy(names[n++], my_name);
        for (int i = 0; i < cmd->n_members; i++) {
            bool dup = false;
            for (int j = 0; j < n; j++)
                dup = dup || strcmp(names[j], cmd->members[i]) == 0;
            if (!dup)
                strcpy(names[n++], cmd->members[i]);
        }
        char list[(CMD_MAX_MEMBERS + 1) * (NAME_BUF + 2)];
        join_names(list, sizeof list, names, n);
        ui_print("Created %s with %s.", cmd->name, list);
        break;
    }
    case CMD_GROUP_ADD:
        ui_print("Added %s to %s.", cmd->user, cmd->name);
        break;
    case CMD_GROUP_SEND:
        ui_print("Sent to %s (#%llu).", cmd->name, id);
        break;
    case CMD_GROUP_LEAVE:
        if (strcmp(text, "dissolved") == 0)
            ui_print("Left %s (group dissolved).", cmd->name);
        else
            ui_print("Left %s.", cmd->name);
        break;
    default:
        ui_print("OK: %s", text);
    }
}

/* Handles a reply frame. Returns the notify byte ('R'/'E'), or 0 if it was
 * not a final reply (HISTORY_ITEM) or no command was waiting. */
static char on_reply(const frame_t *f, reader_t *r)
{
    if (f->type == T_HISTORY_ITEM) {
        on_history_item(r);
        return 0;
    }

    cmd_t cmd;
    bool waiting = pending_take(&cmd);
    char result = 'R';

    switch (f->type) {
    case T_LOGIN_OK: {
        uint32_t n = rd_u32(r);
        if (n == 0)
            ui_print("Connected as %s. 0 unread message(s).", my_name);
        else
            ui_print("Connected as %s. %u unread message(s):", my_name, n);
        break;
    }
    case T_OK: {
        char text[256];
        uint64_t ref = rd_u64(r);
        rd_str(r, text, sizeof text);
        if (waiting)
            print_ok(&cmd, ref, text);
        break;
    }
    case T_ERROR: {
        char text[256];
        rd_u8(r);
        rd_str(r, text, sizeof text);
        ui_print("Error: %s", text);
        result = 'E';
        break;
    }
    case T_HISTORY_END:
        if (waiting)
            print_history(&cmd);
        history_count = 0;
        break;
    case T_USER_LIST: {
        char names[64][NAME_BUF], list[64 * (NAME_BUF + 2)];
        int n = read_name_list(r, names, 64);
        if (n >= 0) {
            join_names(list, sizeof list, names, n);
            ui_print("Online (%d): %s", n, list);
        }
        break;
    }
    case T_GROUP_LIST_RESULT: {
        static char names[256][NAME_BUF];
        static char list[256 * (NAME_BUF + 2)];
        int n = read_name_list(r, names, 256);
        if (n == 0)
            ui_print("You are not in any groups.");
        else if (n > 0) {
            join_names(list, sizeof list, names, n);
            ui_print("Your groups: %s", list);
        }
        break;
    }
    }
    return waiting ? result : 0;
}

/* ======================= the thread ======================= */

static void *receiver_main(void *arg)
{
    (void)arg;
    for (;;) {
        frame_t *f = recv_frame(server_fd);
        if (f == NULL) {
            /* EOF or error: report it unless we are disconnecting on purpose. */
            pthread_mutex_lock(&ctx_mutex);
            bool quiet = stopping;
            if (!quiet)
                server_closed = true;
            pthread_mutex_unlock(&ctx_mutex);
            if (!quiet) {
                ui_print("Connection to server lost.");
                notify('Q');
            }
            break;
        }

        reader_t r;
        reader_init(&r, f);
        uint8_t type = f->type;
        bool stop = false;
        switch (type) {
        case T_MSG:         on_msg(&r); break;
        case T_RECEIPT:     on_receipt(&r); break;
        case T_EXPIRED:     on_expired(&r); break;
        case T_GROUP_EVENT:
        case T_SHUTDOWN: {
            char text[256];
            rd_str(&r, text, sizeof text);
            ui_print("%s", text);
            if (type == T_SHUTDOWN) {
                pthread_mutex_lock(&ctx_mutex);
                server_closed = true;
                pthread_mutex_unlock(&ctx_mutex);
                notify('Q');
                stop = true;
            }
            break;
        }
        default: {
            pthread_mutex_lock(&ctx_mutex);
            bool was_login = pending.active && pending.cmd.type == CMD_CONNECT;
            pthread_mutex_unlock(&ctx_mutex);
            char result = on_reply(f, &r);
            if (result != 0)
                notify(result);
            /* A failed login: the server closes the connection next. */
            if (result == 'E' && was_login)
                stop = true;
        }
        }
        free(f);
        if (stop)
            break;
    }
    return NULL;
}

void receiver_start(int fd, const char *me)
{
    /* Reset per-connection state BEFORE the thread exists (no race). */
    server_fd = fd;
    snprintf(my_name, sizeof my_name, "%s", me);
    seen_count = seen_next = 0;
    history_count = 0;
    pthread_mutex_lock(&ctx_mutex);
    stopping = false;
    server_closed = false;
    pthread_mutex_unlock(&ctx_mutex);
    pthread_create(&receiver_thread, NULL, receiver_main, NULL);
}

void receiver_join(void) { pthread_join(receiver_thread, NULL); }
