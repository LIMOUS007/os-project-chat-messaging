/*
 * net.h — socket helpers shared by the server and the client.
 *
 * TCP is a byte stream: one recv() may return half a frame or several frames,
 * and one send() may write only part of a buffer. These helpers hide that.
 * Nothing in this module takes a lock; callers that share a socket between
 * threads (the client) must serialise sends themselves.
 */
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/* Sends all n bytes on a blocking socket, retrying after partial writes and
 * EINTR. Uses MSG_NOSIGNAL so a closed peer gives an error, not SIGPIPE.
 * Returns 0 on success, -1 on error. */
int send_all(int fd, const void *buf, size_t n);

/* Sends a finished frame (fb_finish must have succeeded). Returns 0 / -1. */
int send_frame(int fd, const frame_builder_t *fb);

/* Receives exactly n bytes on a blocking socket.
 * Returns 1 on success, 0 if the peer closed before the first byte,
 * -1 on error or if the peer closed in the middle. */
int recv_all(int fd, void *buf, size_t n);

/* Blocking: receives one whole frame (header first, wire_len checked BEFORE
 * any allocation, then the body). Returns a malloc'd frame, or NULL on EOF,
 * error or framing error. Used by the client, tests and tools. */
frame_t *recv_frame(int fd);

/* ------------------------------------------------------------------------
 * Incremental frame parser for non-blocking sockets (used by the server's
 * event loop). It never reads more than the rest of the current frame, so
 * extra frames stay in the kernel's socket buffer until the server has room
 * for them (SPEC §6.4 backpressure).
 * ------------------------------------------------------------------------ */
typedef struct {
    uint8_t  buf[HEADER_SIZE + MAX_WIRE_LEN]; /* header, then type + payload */
    uint32_t have;                            /* bytes of the current frame we hold */
    uint32_t need;                            /* bytes the current frame has in total */
} frame_parser_t;

typedef enum {
    PARSE_FRAME,   /* *out holds one complete frame                     */
    PARSE_AGAIN,   /* no more data right now (EAGAIN)                   */
    PARSE_CLOSED,  /* the peer closed the connection                    */
    PARSE_ERROR,   /* socket error, or framing error (bad wire_len,
                      or the peer closed in the middle of a frame)      */
} parse_result_t;

void parser_reset(frame_parser_t *p);

/* Reads from a non-blocking fd until one frame is complete or no data is
 * left. Each recv() asks only for the bytes the current frame still needs. */
parse_result_t parser_read(frame_parser_t *p, int fd, frame_t **out);

/* Makes fd non-blocking. Returns 0 / -1. */
int set_nonblocking(int fd);

/* Creates a listening TCP socket on 0.0.0.0:port (SO_REUSEADDR).
 * Returns the fd, or -1. */
int tcp_listen(int port);

/* Resolves host with getaddrinfo and connects. Returns the fd, or -1. */
int tcp_connect(const char *host, int port);

#endif
