/*
 * stress.c — the stress test (SPEC §11.4).
 *
 *   stress PORT            20 bots (bot00..bot19) each send 100 DMs to random
 *                          other bots at the same time. Checks that every
 *                          message is displayed exactly once, that every send
 *                          got an OK and that every DM got a read receipt.
 *   stress --verify DB     after the server has stopped: every message is
 *                          stored exactly once and every DM is in state seen.
 *
 * Each bot has two threads, like the real client: a sender and a receiver
 * (which ACKs and MARK_SEENs every message). The socket is shared, so sends
 * are serialised with a per-bot mutex.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "net.h"
#include "protocol.h"
#include "sqlite3.h"

#define BOTS          20
#define MSGS_PER_BOT  100
#define TOTAL         (BOTS * MSGS_PER_BOT)
#define TIMEOUT_SEC   90

typedef struct {
    int             index;
    char            name[NAME_BUF];
    int             fd;
    pthread_mutex_t send_mutex;   /* sender + receiver share the socket */
    pthread_t       sender, receiver;
} bot_t;

static bot_t bots[BOTS];
static int   port;
static pthread_barrier_t all_logged_in;

/* Statistics, updated by many threads. */
static pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
static int        *displayed;        /* displayed[message_number] (sender*100 + i) */
static atomic_int  oks, receipts, msgs, errors;

static void bot_send(bot_t *b, const frame_builder_t *fb)
{
    pthread_mutex_lock(&b->send_mutex);
    send_frame(b->fd, fb);
    pthread_mutex_unlock(&b->send_mutex);
}

static void bot_send_id(bot_t *b, uint8_t type, uint64_t id)
{
    frame_builder_t fb;
    fb_begin(&fb, type);
    fb_u64(&fb, id);
    fb_finish(&fb);
    bot_send(b, &fb);
}

static void *receiver_main(void *arg)
{
    bot_t *b = arg;
    uint64_t *seen = malloc(sizeof(uint64_t) * TOTAL);
    int seen_n = 0;
    frame_t *f;
    while ((f = recv_frame(b->fd)) != NULL) {
        reader_t r;
        reader_init(&r, f);
        if (f->type == T_MSG) {
            char from[NAME_BUF], group[NAME_BUF], text[TEXT_BUF];
            uint64_t id = rd_u64(&r);
            rd_str(&r, from, sizeof from);
            rd_str(&r, group, sizeof group);
            rd_str(&r, text, sizeof text);
            bool dup = false;
            for (int i = 0; i < seen_n; i++)
                dup = dup || seen[i] == id;
            if (!dup) {
                seen[seen_n++] = id;
                int number;
                if (sscanf(text, "stress %d", &number) == 1 && number >= 0 && number < TOTAL) {
                    pthread_mutex_lock(&stats_mutex);
                    displayed[number]++;
                    pthread_mutex_unlock(&stats_mutex);
                }
                atomic_fetch_add(&msgs, 1);
            }
            bot_send_id(b, T_ACK, id);
            bot_send_id(b, T_MARK_SEEN, id);
        } else if (f->type == T_OK) {
            atomic_fetch_add(&oks, 1);
        } else if (f->type == T_RECEIPT) {
            atomic_fetch_add(&receipts, 1);
        } else if (f->type == T_ERROR) {
            atomic_fetch_add(&errors, 1);
        }
        free(f);
    }
    free(seen);
    return NULL;
}

static void *sender_main(void *arg)
{
    bot_t *b = arg;
    unsigned int seed = (unsigned int)b->index * 7919u + 1u;

    b->fd = tcp_connect("127.0.0.1", port);
    if (b->fd < 0) {
        perror("connect");
        exit(2);
    }
    frame_builder_t fb;
    fb_begin(&fb, T_LOGIN);
    fb_str(&fb, b->name);
    fb_finish(&fb);
    send_frame(b->fd, &fb);
    frame_t *f = recv_frame(b->fd);
    if (f == NULL || f->type != T_LOGIN_OK) {
        fprintf(stderr, "%s: login failed\n", b->name);
        exit(2);
    }
    free(f);
    pthread_create(&b->receiver, NULL, receiver_main, b);
    pthread_barrier_wait(&all_logged_in);     /* everyone online: all sends are live */

    for (int i = 0; i < MSGS_PER_BOT; i++) {
        int to = (int)(rand_r(&seed) % (BOTS - 1));
        if (to >= b->index)
            to++;                             /* never ourselves */
        char text[64];
        snprintf(text, sizeof text, "stress %d", b->index * MSGS_PER_BOT + i);
        fb_begin(&fb, T_SEND);
        fb_str(&fb, bots[to].name);
        fb_str(&fb, text);
        fb_finish(&fb);
        bot_send(b, &fb);
    }
    return NULL;
}

static int run_bots(void)
{
    displayed = calloc(TOTAL, sizeof(int));
    pthread_barrier_init(&all_logged_in, NULL, BOTS);
    for (int i = 0; i < BOTS; i++) {
        bots[i].index = i;
        snprintf(bots[i].name, sizeof bots[i].name, "bot%02d", i);
        pthread_mutex_init(&bots[i].send_mutex, NULL);
    }
    for (int i = 0; i < BOTS; i++)
        pthread_create(&bots[i].sender, NULL, sender_main, &bots[i]);
    for (int i = 0; i < BOTS; i++)
        pthread_join(bots[i].sender, NULL);

    /* Wait until every OK, every MSG and every receipt has arrived. */
    time_t start = time(NULL);
    while ((atomic_load(&oks) < TOTAL || atomic_load(&msgs) < TOTAL ||
            atomic_load(&receipts) < TOTAL) && time(NULL) - start < TIMEOUT_SEC) {
        struct timespec pause = { 0, 50 * 1000 * 1000 };
        nanosleep(&pause, NULL);
    }
    struct timespec settle = { 0, 300 * 1000 * 1000 };   /* catch late duplicates */
    nanosleep(&settle, NULL);

    /* Log out every bot, then wait for their receivers to see EOF. */
    for (int i = 0; i < BOTS; i++) {
        frame_builder_t fb;
        fb_begin(&fb, T_LOGOUT);
        fb_finish(&fb);
        bot_send(&bots[i], &fb);
    }
    for (int i = 0; i < BOTS; i++) {
        pthread_join(bots[i].receiver, NULL);
        close(bots[i].fd);
    }

    int never = 0, twice = 0;
    for (int i = 0; i < TOTAL; i++) {
        if (displayed[i] == 0)
            never++;
        else if (displayed[i] > 1)
            twice++;
    }
    printf("stress: %d messages, %d OK, %d displayed, %d receipts, %d errors\n", TOTAL,
           atomic_load(&oks), atomic_load(&msgs), atomic_load(&receipts), atomic_load(&errors));
    printf("stress: %d never displayed, %d displayed more than once\n", never, twice);
    bool ok = never == 0 && twice == 0 && atomic_load(&oks) == TOTAL &&
              atomic_load(&receipts) == TOTAL && atomic_load(&errors) == 0;
    printf(ok ? "stress: PASS\n" : "stress: FAIL\n");
    free(displayed);
    return ok ? 0 : 1;
}

static int query_int(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        value = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return value;
}

static int verify_db(const char *path)
{
    sqlite3 *db;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        printf("verify: cannot open %s\n", path);
        return 1;
    }
    int stored = query_int(db, "SELECT COUNT(*) FROM messages");
    int distinct = query_int(db, "SELECT COUNT(DISTINCT body) FROM messages");
    int seen = query_int(db, "SELECT COUNT(*) FROM messages WHERE state = 2");
    sqlite3_close(db);
    printf("verify: %d stored, %d distinct, %d seen (expected %d each)\n", stored, distinct,
           seen, TOTAL);
    bool ok = stored == TOTAL && distinct == TOTAL && seen == TOTAL;
    printf(ok ? "verify: PASS\n" : "verify: FAIL\n");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 3 && strcmp(argv[1], "--verify") == 0)
        return verify_db(argv[2]);
    if (argc != 2) {
        fprintf(stderr, "usage: stress PORT | stress --verify DB\n");
        return 2;
    }
    port = atoi(argv[1]);
    return run_bots();
}
