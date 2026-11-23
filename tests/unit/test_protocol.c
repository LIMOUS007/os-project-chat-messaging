/*
 * test_protocol.c — U1 (encode/decode round trips, u64 byte order, string
 * bounds) and U2 (bad lengths and truncated payloads).
 */
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"
#include "protocol.h"
#include "test.h"

/* Turns a finished builder into a frame_t, as if it had been received. */
static frame_t *as_frame(const frame_builder_t *fb)
{
    return frame_new(fb->buf[HEADER_SIZE], fb->buf + HEADER_SIZE + 1,
                     (uint32_t)(fb->used - HEADER_SIZE - 1));
}

static void test_example_from_spec(void)
{
    /* SPEC §4.1: SEND(to="bob", text="hi") */
    frame_builder_t fb;
    fb_begin(&fb, T_SEND);
    fb_str(&fb, "bob");
    fb_str(&fb, "hi");
    CHECK(fb_finish(&fb));
    const uint8_t expected[] = { 0, 0, 0, 0x0A, 0x02, 0, 3, 'b', 'o', 'b', 0, 2, 'h', 'i' };
    CHECK_EQ(fb.used, sizeof expected);
    CHECK(memcmp(fb.buf, expected, sizeof expected) == 0);
}

static void test_u64_byte_order(void)
{
    frame_builder_t fb;
    fb_begin(&fb, T_ACK);
    fb_u64(&fb, 0x0102030405060708ULL);
    fb_finish(&fb);
    const uint8_t expected[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(memcmp(fb.buf + HEADER_SIZE + 1, expected, 8) == 0);   /* big-endian */
    frame_t *f = as_frame(&fb);
    reader_t r;
    reader_init(&r, f);
    CHECK(rd_u64(&r) == 0x0102030405060708ULL);
    CHECK(rd_ok(&r));
    free(f);
}

static void test_msg_round_trip(void)
{
    frame_builder_t fb;
    enc_msg(&fb, 42, "alice", "#os", "hello", 1234567890123ULL);
    frame_t *f = as_frame(&fb);
    CHECK_EQ(f->type, T_MSG);
    reader_t r;
    reader_init(&r, f);
    char from[NAME_BUF], group[NAME_BUF], text[TEXT_BUF];
    CHECK(rd_u64(&r) == 42);
    rd_str(&r, from, sizeof from);
    rd_str(&r, group, sizeof group);
    rd_str(&r, text, sizeof text);
    CHECK(rd_u64(&r) == 1234567890123ULL);
    CHECK(rd_ok(&r));
    CHECK(strcmp(from, "alice") == 0 && strcmp(group, "#os") == 0 && strcmp(text, "hello") == 0);
    free(f);
}

static void test_every_server_frame(void)
{
    frame_builder_t fb;
    reader_t r;
    frame_t *f;
    char s1[TEXT_BUF], s2[TEXT_BUF], s3[TEXT_BUF];

    enc_login_ok(&fb, 7);
    f = as_frame(&fb); reader_init(&r, f);
    CHECK(f->type == T_LOGIN_OK && rd_u32(&r) == 7 && rd_ok(&r));
    free(f);

    enc_ok(&fb, 99, "sent");
    f = as_frame(&fb); reader_init(&r, f);
    CHECK(rd_u64(&r) == 99); rd_str(&r, s1, sizeof s1);
    CHECK(f->type == T_OK && rd_ok(&r) && strcmp(s1, "sent") == 0);
    free(f);

    enc_error(&fb, E_UNKNOWN_USER, "unknown user 'x'");
    f = as_frame(&fb); reader_init(&r, f);
    CHECK(rd_u8(&r) == E_UNKNOWN_USER); rd_str(&r, s1, sizeof s1);
    CHECK(f->type == T_ERROR && rd_ok(&r) && strcmp(s1, "unknown user 'x'") == 0);
    free(f);

    enc_history_item(&fb, 5, "bob", "alice", "hey", STATUS_SEEN, 100, 200);
    f = as_frame(&fb); reader_init(&r, f);
    CHECK(rd_u64(&r) == 5);
    rd_str(&r, s1, sizeof s1); rd_str(&r, s2, sizeof s2); rd_str(&r, s3, sizeof s3);
    CHECK(rd_u8(&r) == STATUS_SEEN && rd_u64(&r) == 100 && rd_u64(&r) == 200);
    CHECK(f->type == T_HISTORY_ITEM && rd_ok(&r) && strcmp(s3, "hey") == 0);
    free(f);

    enc_history_end(&fb, 3);
    f = as_frame(&fb); reader_init(&r, f);
    CHECK(f->type == T_HISTORY_END && rd_u32(&r) == 3 && rd_ok(&r));
    free(f);

    char names[2][NAME_BUF] = { "alice", "bob" };
    enc_name_list(&fb, T_USER_LIST, names, 2);
    f = as_frame(&fb); reader_init(&r, f);
    CHECK(rd_u16(&r) == 2); rd_str(&r, s1, sizeof s1); rd_str(&r, s2, sizeof s2);
    CHECK(f->type == T_USER_LIST && rd_ok(&r) && strcmp(s2, "bob") == 0);
    free(f);

    enc_receipt(&fb, 42, "bob", "hi");
    f = as_frame(&fb); reader_init(&r, f);
    CHECK(rd_u64(&r) == 42); rd_str(&r, s1, sizeof s1); rd_str(&r, s2, sizeof s2);
    CHECK(f->type == T_RECEIPT && rd_ok(&r) && strcmp(s1, "bob") == 0);
    free(f);

    enc_expired(&fb, 8, "alice");
    f = as_frame(&fb); reader_init(&r, f);
    CHECK(rd_u64(&r) == 8); rd_str(&r, s1, sizeof s1);
    CHECK(f->type == T_EXPIRED && rd_ok(&r));
    free(f);

    enc_text_frame(&fb, T_SHUTDOWN, "Server shutting down...");
    f = as_frame(&fb); reader_init(&r, f);
    rd_str(&r, s1, sizeof s1);
    CHECK(f->type == T_SHUTDOWN && rd_ok(&r) && strcmp(s1, "Server shutting down...") == 0);
    free(f);
}

static void test_string_bounds(void)
{
    /* A 33-character name does not fit a NAME_BUF of 34? It does (33 + NUL);
     * a name buffer of 33 must reject it. */
    frame_builder_t fb;
    fb_begin(&fb, T_LOGIN);
    fb_str(&fb, "abcdefghijabcdefghijabcdefghijabc");   /* 33 chars */
    fb_finish(&fb);
    frame_t *f = as_frame(&fb);
    reader_t r;
    reader_init(&r, f);
    char small[33];
    rd_str(&r, small, sizeof small);
    CHECK(r.err);
    free(f);

    /* Text longer than MAX_TEXT -> too_long (not err) */
    char big[1100];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    fb_begin(&fb, T_SEND);
    fb_str(&fb, "bob");
    fb_str(&fb, big);
    CHECK(fb_finish(&fb));
    f = as_frame(&fb);
    reader_init(&r, f);
    char to[NAME_BUF], text[TEXT_BUF];
    rd_str(&r, to, sizeof to);
    rd_text(&r, text);
    CHECK(!r.err && r.too_long && !rd_ok(&r));
    free(f);

    /* Frame bigger than MAX_WIRE_LEN cannot be built */
    fb_begin(&fb, T_SEND);
    for (int i = 0; i < 9; i++)
        fb_str(&fb, big);
    CHECK(!fb_finish(&fb));

    /* Names */
    CHECK(valid_username("alice_01"));
    CHECK(!valid_username(""));
    CHECK(!valid_username("bad name"));
    CHECK(!valid_username("abcdefghijabcdefghijabcdefghijabc"));
    CHECK(valid_groupname("#os"));
    CHECK(!valid_groupname("#"));
    CHECK(!valid_groupname("os"));
    CHECK(!valid_groupname("#bad-name"));

    char preview[PREVIEW_MAX + 1];
    make_preview("short", preview);
    CHECK(strcmp(preview, "short") == 0);
    make_preview("this is a rather long message that goes on", preview);
    CHECK(strlen(preview) <= PREVIEW_MAX && strstr(preview, "...") != NULL);
}

/* U2: truncated payloads and bad lengths. */
static void test_bad_input(void)
{
    /* Truncated: SEND with a string length that runs past the end. */
    const uint8_t payload[] = { 0, 10, 'b', 'o' };
    frame_t *f = frame_new(T_SEND, payload, sizeof payload);
    reader_t r;
    reader_init(&r, f);
    char to[NAME_BUF];
    rd_str(&r, to, sizeof to);
    CHECK(r.err);
    CHECK(rd_u64(&r) == 0 && r.err);   /* reading on after an error stays safe */
    free(f);

    /* Leftover bytes are also an error */
    const uint8_t extra[] = { 0, 1, 'a', 0xFF };
    f = frame_new(T_LOGIN, extra, sizeof extra);
    reader_init(&r, f);
    rd_str(&r, to, sizeof to);
    CHECK(!r.err && !rd_ok(&r));
    free(f);

    /* wire_len = 0 and wire_len > 8192 are framing errors (checked before
     * any allocation), for both the blocking and the incremental reader. */
    uint32_t bad_lengths[] = { 0, MAX_WIRE_LEN + 1, 0xFFFFFFFFu };
    for (int i = 0; i < 3; i++) {
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        uint32_t be = htobe32(bad_lengths[i]);
        write(sv[0], &be, 4);
        CHECK(recv_frame(sv[1]) == NULL);
        close(sv[0]);
        close(sv[1]);

        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        set_nonblocking(sv[1]);
        write(sv[0], &be, 4);
        frame_parser_t p;
        parser_reset(&p);
        frame_t *out;
        CHECK_EQ(parser_read(&p, sv[1], &out), PARSE_ERROR);
        CHECK(out == NULL);
        close(sv[0]);
        close(sv[1]);
    }

    /* Connection closed in the middle of a frame = framing error. */
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    set_nonblocking(sv[1]);
    const uint8_t half[] = { 0, 0, 0, 5, T_ACK, 1 };
    write(sv[0], half, sizeof half);
    close(sv[0]);
    frame_parser_t p;
    parser_reset(&p);
    frame_t *out;
    CHECK_EQ(parser_read(&p, sv[1], &out), PARSE_ERROR);
    close(sv[1]);
}

int main(void)
{
    test_example_from_spec();
    test_u64_byte_order();
    test_msg_round_trip();
    test_every_server_frame();
    test_string_bounds();
    test_bad_input();
    return TEST_SUMMARY();
}
