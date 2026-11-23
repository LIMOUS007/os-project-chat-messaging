/*
 * net.c — partial-transfer-safe send/recv, frame reading, socket setup.
 */
#include "net.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int send_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

int send_frame(int fd, const frame_builder_t *fb)
{
    if (fb->err)
        return -1;
    return send_all(fd, fb->buf, fb->used);
}

int recv_all(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t k = recv(fd, p + got, n - got, 0);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (k == 0)                    /* peer closed */
            return got == 0 ? 0 : -1;
        got += (size_t)k;
    }
    return 1;
}

frame_t *recv_frame(int fd)
{
    uint32_t be;
    if (recv_all(fd, &be, sizeof be) != 1)
        return NULL;
    uint32_t wire_len = be32toh(be);
    if (wire_len < 1 || wire_len > MAX_WIRE_LEN)   /* checked before malloc */
        return NULL;

    uint8_t body[MAX_WIRE_LEN];
    if (recv_all(fd, body, wire_len) != 1)
        return NULL;
    return frame_new(body[0], body + 1, wire_len - 1);
}

/* ======================= incremental parser ======================= */

void parser_reset(frame_parser_t *p)
{
    p->have = 0;
    p->need = HEADER_SIZE;   /* first we need the 4-byte header */
}

parse_result_t parser_read(frame_parser_t *p, int fd, frame_t **out)
{
    *out = NULL;
    for (;;) {
        /* Ask only for what the current frame still needs. */
        ssize_t k = recv(fd, p->buf + p->have, p->need - p->have, 0);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return PARSE_AGAIN;
            return PARSE_ERROR;
        }
        if (k == 0)   /* closed: clean only if we were between frames */
            return p->have == 0 ? PARSE_CLOSED : PARSE_ERROR;
        p->have += (uint32_t)k;
        if (p->have < p->need)
            continue;

        if (p->need == HEADER_SIZE) {
            /* Header complete: validate wire_len before using it. */
            uint32_t be;
            memcpy(&be, p->buf, sizeof be);
            uint32_t wire_len = be32toh(be);
            if (wire_len < 1 || wire_len > MAX_WIRE_LEN)
                return PARSE_ERROR;
            p->need = HEADER_SIZE + wire_len;
            continue;
        }

        /* Body complete: buf = [header][type][payload]. */
        uint32_t payload_len = p->need - HEADER_SIZE - 1;
        *out = frame_new(p->buf[HEADER_SIZE], p->buf + HEADER_SIZE + 1, payload_len);
        parser_reset(p);
        return *out != NULL ? PARSE_FRAME : PARSE_ERROR;
    }
}

/* ======================= socket setup ======================= */

int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int tcp_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 64) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int tcp_connect(const char *host, int port)
{
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return -1;

    int fd = -1;
    for (struct addrinfo *a = res; a != NULL; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}
