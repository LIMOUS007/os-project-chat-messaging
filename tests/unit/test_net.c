/*
 * test_net.c — U3: frames survive TCP-style splitting and joining.
 *   - recv_frame over a socketpair with data written 1 byte at a time
 *   - two frames in one write
 *   - the incremental parser with 20 frames in one write and an inbox of 16:
 *     it stops at 16, the rest stay in the kernel, and all 20 arrive in order
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"
#include "protocol.h"
#include "test.h"

static frame_builder_t slow_frame;

static void *write_one_byte_at_a_time(void *arg)
{
    int fd = *(int *)arg;
    for (size_t i = 0; i < slow_frame.used; i++) {
        write(fd, &slow_frame.buf[i], 1);
        usleep(200);
    }
    return NULL;
}

static void test_byte_at_a_time(void)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    enc_msg(&slow_frame, 7, "alice", "", "split into pieces", 0);
    pthread_t t;
    pthread_create(&t, NULL, write_one_byte_at_a_time, &sv[0]);
    frame_t *f = recv_frame(sv[1]);
    pthread_join(t, NULL);
    CHECK(f != NULL);
    if (f) {
        reader_t r;
        reader_init(&r, f);
        CHECK(f->type == T_MSG && rd_u64(&r) == 7);
        free(f);
    }
    close(sv[0]);
    close(sv[1]);
}

static void test_two_frames_one_write(void)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    frame_builder_t a, b;
    enc_ok(&a, 1, "first");
    enc_ok(&b, 2, "second");
    uint8_t both[2 * sizeof a.buf];
    memcpy(both, a.buf, a.used);
    memcpy(both + a.used, b.buf, b.used);
    write(sv[0], both, a.used + b.used);

    frame_t *f1 = recv_frame(sv[1]);
    frame_t *f2 = recv_frame(sv[1]);
    CHECK(f1 != NULL && f2 != NULL);
    if (f1 && f2) {
        reader_t r1, r2;
        reader_init(&r1, f1);
        reader_init(&r2, f2);
        CHECK(rd_u64(&r1) == 1 && rd_u64(&r2) == 2);
    }
    free(f1);
    free(f2);
    close(sv[0]);
    close(sv[1]);
}

/* The incremental parser never reads past the current frame. */
static void test_parser_bounded_reads(void)
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    set_nonblocking(sv[1]);

    uint8_t all[20 * 64];
    size_t used = 0;
    for (int i = 0; i < 20; i++) {
        frame_builder_t fb;
        fb_begin(&fb, T_ACK);
        fb_u64(&fb, (uint64_t)i);
        fb_finish(&fb);
        memcpy(all + used, fb.buf, fb.used);
        used += fb.used;
    }
    write(sv[0], all, used);   /* 20 frames in one write */

    frame_parser_t p;
    parser_reset(&p);
    frame_t *inbox[20];
    int count = 0;
    while (count < 16) {                          /* "inbox" capacity 16 */
        frame_t *f;
        if (parser_read(&p, sv[1], &f) != PARSE_FRAME)
            break;
        inbox[count++] = f;
    }
    CHECK_EQ(count, 16);
    int waiting = 0;
    ioctl(sv[1], FIONREAD, &waiting);
    CHECK(waiting > 0);                           /* the other 4 are still in the kernel */

    frame_t *f;
    while (count < 20 && parser_read(&p, sv[1], &f) == PARSE_FRAME)
        inbox[count++] = f;
    CHECK_EQ(count, 20);
    CHECK_EQ(parser_read(&p, sv[1], &f), PARSE_AGAIN);

    for (int i = 0; i < count; i++) {
        reader_t r;
        reader_init(&r, inbox[i]);
        CHECK_EQ(rd_u64(&r), i);                  /* in order */
        free(inbox[i]);
    }

    close(sv[0]);
    CHECK_EQ(parser_read(&p, sv[1], &f), PARSE_CLOSED);   /* clean EOF between frames */
    close(sv[1]);
}

int main(void)
{
    test_byte_at_a_time();
    test_two_frames_one_write();
    test_parser_bounded_reads();
    return TEST_SUMMARY();
}
