/*
 * rawbot.c — a raw-protocol test client for the failure tests (SPEC §11.3).
 * It does things the normal CLI client never does on purpose.
 *
 *   rawbot PORT noack USER             log in, print each MSG, never ACK (X1)
 *   rawbot PORT noread USER            log in with a tiny receive buffer, then
 *                                      never read again (X2)
 *   rawbot PORT sendmany USER TO N SIZE  send N DMs of SIZE bytes, reading the
 *                                      replies; prints "sent N ok K" (X2)
 *   rawbot PORT flood USER N           send N GET_USERS without reading any
 *                                      reply (X9)
 *   rawbot PORT loginrace N            alice sends while bob logs in, N times;
 *                                      every message must be shown once (X4)
 *   rawbot PORT badframe               framing error closes the connection;
 *                                      bad payload keeps it open
 *   rawbot PORT idle                   connect, never log in: prints when the
 *                                      server closes it (login timeout)
 */
#include <endian.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "net.h"
#include "protocol.h"

static int port;

static int connect_or_die(int rcvbuf)
{
    int fd;
    if (rcvbuf > 0) {
        /* The receive buffer must be set before connect to take effect. */
        fd = socket(AF_INET, SOCK_STREAM, 0);
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
        close(fd);
    }
    fd = tcp_connect("127.0.0.1", port);
    if (fd < 0) {
        perror("connect");
        exit(2);
    }
    if (rcvbuf > 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    return fd;
}

static void send_login(int fd, const char *user)
{
    frame_builder_t fb;
    fb_begin(&fb, T_LOGIN);
    fb_str(&fb, user);
    fb_finish(&fb);
    send_frame(fd, &fb);
}

static void send_id(int fd, uint8_t type, uint64_t id)
{
    frame_builder_t fb;
    fb_begin(&fb, type);
    fb_u64(&fb, id);
    fb_finish(&fb);
    send_frame(fd, &fb);
}

/* Logs in and waits for LOGIN_OK (other frames before it are ignored). */
static int login_or_die(const char *user, int rcvbuf)
{
    int fd = connect_or_die(rcvbuf);
    send_login(fd, user);
    for (;;) {
        frame_t *f = recv_frame(fd);
        if (f == NULL) {
            fprintf(stderr, "login as %s failed\n", user);
            exit(2);
        }
        uint8_t type = f->type;
        free(f);
        if (type == T_LOGIN_OK)
            return fd;
        if (type == T_ERROR) {
            fprintf(stderr, "login as %s rejected\n", user);
            exit(2);
        }
    }
}

/* Waits up to timeout_ms for the fd to become readable. */
static bool readable(int fd, int timeout_ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    return poll(&p, 1, timeout_ms) > 0;
}

/* ---------------- modes ---------------- */

static int mode_noack(const char *user)
{
    int fd = login_or_die(user, 0);
    printf("logged in\n");
    fflush(stdout);
    for (;;) {
        frame_t *f = recv_frame(fd);
        if (f == NULL)
            return 0;
        if (f->type == T_MSG) {
            reader_t r;
            reader_init(&r, f);
            char from[NAME_BUF], group[NAME_BUF], text[TEXT_BUF];
            uint64_t id = rd_u64(&r);
            rd_str(&r, from, sizeof from);
            rd_str(&r, group, sizeof group);
            rd_str(&r, text, sizeof text);
            printf("MSG %llu %s\n", (unsigned long long)id, text);   /* no ACK! */
            fflush(stdout);
        }
        free(f);
    }
}

static int mode_noread(const char *user)
{
    int fd = login_or_die(user, 2048);
    printf("logged in, not reading\n");
    fflush(stdout);
    for (;;)
        pause();                       /* killed by the test */
    close(fd);
    return 0;
}

typedef struct {
    int fd;
    int expected;
    int replies;
} reply_counter_t;

static void *count_replies(void *arg)
{
    reply_counter_t *c = arg;
    while (c->replies < c->expected) {
        frame_t *f = recv_frame(c->fd);
        if (f == NULL)
            break;
        if (f->type == T_OK || f->type == T_ERROR)
            c->replies++;
        free(f);
    }
    return NULL;
}

static int mode_sendmany(const char *user, const char *to, int n, int size)
{
    int fd = login_or_die(user, 0);
    reply_counter_t counter = { fd, n, 0 };
    pthread_t t;
    pthread_create(&t, NULL, count_replies, &counter);

    char *text = malloc((size_t)size + 1);
    memset(text, 'z', (size_t)size);
    text[size] = '\0';
    int sent = 0;
    for (; sent < n; sent++) {
        frame_builder_t fb;
        fb_begin(&fb, T_SEND);
        fb_str(&fb, to);
        fb_str(&fb, text);
        fb_finish(&fb);
        if (send_frame(fd, &fb) != 0)
            break;
    }
    pthread_join(t, NULL);
    printf("sent %d ok %d\n", sent, counter.replies);
    free(text);
    close(fd);
    return 0;
}

static int mode_flood(const char *user, int n)
{
    int fd = login_or_die(user, 2048);
    set_nonblocking(fd);
    frame_builder_t fb;
    fb_begin(&fb, T_GET_USERS);
    fb_finish(&fb);
    int sent = 0;
    time_t start = time(NULL);
    /* Never read the replies. Our sends block (backpressure) once the
     * server stops reading us; give up after 20 s. */
    while (sent < n && time(NULL) - start < 20) {
        ssize_t k = send(fd, fb.buf, fb.used, MSG_NOSIGNAL);
        if (k == (ssize_t)fb.used) {
            sent++;
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            poll(&p, 1, 100);
            continue;
        }
        break;   /* disconnected by the server (or a partial write) */
    }
    printf("flood sent %d of %d\n", sent, n);
    fflush(stdout);
    close(fd);
    return 0;
}

/* Collects bob's MSGs for one session (in-session dedupe, like the client). */
static void bob_session(int *shown, int n, int quiet_ms)
{
    int fd = login_or_die("bob", 0);
    uint64_t seen[4096];
    int seen_n = 0;
    while (readable(fd, quiet_ms)) {
        frame_t *f = recv_frame(fd);
        if (f == NULL)
            break;
        if (f->type == T_MSG) {
            reader_t r;
            reader_init(&r, f);
            char from[NAME_BUF], group[NAME_BUF], text[TEXT_BUF];
            uint64_t id = rd_u64(&r);
            rd_str(&r, from, sizeof from);
            rd_str(&r, group, sizeof group);
            rd_str(&r, text, sizeof text);
            bool dup = false;
            for (int i = 0; i < seen_n; i++)
                dup = dup || seen[i] == id;
            if (!dup) {
                if (seen_n < 4096)
                    seen[seen_n++] = id;
                int idx;
                if (sscanf(text, "race %d", &idx) == 1 && idx >= 0 && idx < n)
                    shown[idx]++;
            }
            send_id(fd, T_ACK, id);
            send_id(fd, T_MARK_SEEN, id);
        }
        free(f);
    }
    frame_builder_t fb;
    fb_begin(&fb, T_LOGOUT);
    fb_finish(&fb);
    send_frame(fd, &fb);
    /* wait for the server to close us, so the next login is not a duplicate */
    while (readable(fd, 2000)) {
        frame_t *f = recv_frame(fd);
        if (f == NULL)
            break;
        free(f);
    }
    close(fd);
}

static void *drain_frames(void *arg)
{
    int fd = *(int *)arg;
    frame_t *f;
    while ((f = recv_frame(fd)) != NULL)
        free(f);
    return NULL;
}

static int mode_loginrace(int n)
{
    int alice = login_or_die("alice", 0);
    pthread_t t;
    pthread_create(&t, NULL, drain_frames, &alice);   /* alice's OKs and receipts */

    int *shown = calloc((size_t)n, sizeof(int));
    for (int i = 0; i < n; i++) {
        frame_builder_t fb;
        char text[32];
        snprintf(text, sizeof text, "race %d", i);
        fb_begin(&fb, T_SEND);
        fb_str(&fb, "bob");
        fb_str(&fb, text);
        fb_finish(&fb);
        send_frame(alice, &fb);          /* the send races with bob's login */
        bob_session(shown, n, 60);
    }
    bob_session(shown, n, 500);          /* pick up anything still pending */

    int bad = 0;
    for (int i = 0; i < n; i++) {
        if (shown[i] != 1) {
            if (bad < 10)
                printf("message %d shown %d times\n", i, shown[i]);
            bad++;
        }
    }
    shutdown(alice, SHUT_RDWR);
    pthread_join(t, NULL);
    close(alice);
    printf(bad == 0 ? "loginrace ok %d\n" : "loginrace FAILED %d\n", bad == 0 ? n : bad);
    free(shown);
    return bad == 0 ? 0 : 1;
}

static int mode_badframe(void)
{
    /* 1. wire_len = 0 is a framing error: the server closes the connection. */
    int fd = connect_or_die(0);
    uint32_t zero = 0;
    send_all(fd, &zero, 4);
    frame_t *f = recv_frame(fd);
    printf(f == NULL ? "zero-length: closed\n" : "zero-length: NOT closed\n");
    free(f);
    close(fd);

    /* 2. wire_len too big: closed as well. */
    fd = connect_or_die(0);
    uint32_t huge = htobe32(100000);
    send_all(fd, &huge, 4);
    f = recv_frame(fd);
    printf(f == NULL ? "too-long: closed\n" : "too-long: NOT closed\n");
    free(f);
    close(fd);

    /* 3. well-framed but bad payload: ERROR(BAD_REQUEST), still open. */
    fd = connect_or_die(0);
    const uint8_t bad_login[] = { 0, 0, 0, 3, T_LOGIN, 0, 9 };   /* string runs past the end */
    send_all(fd, bad_login, sizeof bad_login);
    f = recv_frame(fd);
    if (f && f->type == T_ERROR) {
        reader_t r;
        reader_init(&r, f);
        printf("bad payload: ERROR %d\n", rd_u8(&r));
    } else {
        printf("bad payload: no error\n");
    }
    free(f);
    close(fd);

    /* 4. a command before LOGIN: ERROR(NOT_LOGGED_IN), connection stays open. */
    fd = connect_or_die(0);
    frame_builder_t fb;
    fb_begin(&fb, T_GET_USERS);
    fb_finish(&fb);
    send_frame(fd, &fb);
    f = recv_frame(fd);
    if (f && f->type == T_ERROR) {
        reader_t r;
        reader_init(&r, f);
        printf("before login: ERROR %d\n", rd_u8(&r));
    }
    free(f);
    send_login(fd, "carol");             /* still usable afterwards */
    f = recv_frame(fd);
    printf(f && f->type == T_LOGIN_OK ? "then login: ok\n" : "then login: failed\n");
    free(f);
    close(fd);
    return 0;
}

static int mode_idle(void)
{
    int fd = connect_or_die(0);
    time_t start = time(NULL);
    frame_t *f = recv_frame(fd);       /* blocks until the server closes us */
    free(f);
    printf("closed after %lds\n", (long)(time(NULL) - start));
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: rawbot PORT MODE [args]\n");
        return 2;
    }
    port = atoi(argv[1]);
    const char *mode = argv[2];
    if (strcmp(mode, "noack") == 0 && argc == 4)
        return mode_noack(argv[3]);
    if (strcmp(mode, "noread") == 0 && argc == 4)
        return mode_noread(argv[3]);
    if (strcmp(mode, "sendmany") == 0 && argc == 7)
        return mode_sendmany(argv[3], argv[4], atoi(argv[5]), atoi(argv[6]));
    if (strcmp(mode, "flood") == 0 && argc == 5)
        return mode_flood(argv[3], atoi(argv[4]));
    if (strcmp(mode, "loginrace") == 0 && argc == 4)
        return mode_loginrace(atoi(argv[3]));
    if (strcmp(mode, "badframe") == 0)
        return mode_badframe();
    if (strcmp(mode, "idle") == 0)
        return mode_idle();
    fprintf(stderr, "bad mode or arguments\n");
    return 2;
}
