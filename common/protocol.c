/*
 * protocol.c — frame encoding and decoding (SPEC §4). No locks, no I/O.
 */
#include "protocol.h"

#include <ctype.h>
#include <endian.h>
#include <stdlib.h>
#include <string.h>

frame_t *frame_new(uint8_t type, const uint8_t *payload, uint32_t payload_len)
{
    frame_t *f = malloc(sizeof(frame_t) + payload_len);
    if (f == NULL)
        return NULL;
    f->type = type;
    f->payload_len = payload_len;
    if (payload_len > 0)
        memcpy(f->payload, payload, payload_len);
    return f;
}

/* ======================= building frames ======================= */

/* Appends raw bytes, or marks the builder as failed if they do not fit. */
static void fb_bytes(frame_builder_t *fb, const void *data, size_t n)
{
    if (fb->err || fb->used + n > sizeof(fb->buf)) {
        fb->err = true;
        return;
    }
    memcpy(fb->buf + fb->used, data, n);
    fb->used += n;
}

void fb_begin(frame_builder_t *fb, uint8_t type)
{
    fb->used = HEADER_SIZE;   /* leave room for wire_len, filled in by fb_finish */
    fb->err = false;
    fb_u8(fb, type);
}

void fb_u8(frame_builder_t *fb, uint8_t v) { fb_bytes(fb, &v, 1); }

void fb_u16(frame_builder_t *fb, uint16_t v)
{
    uint16_t be = htobe16(v);
    fb_bytes(fb, &be, sizeof be);
}

void fb_u32(frame_builder_t *fb, uint32_t v)
{
    uint32_t be = htobe32(v);
    fb_bytes(fb, &be, sizeof be);
}

void fb_u64(frame_builder_t *fb, uint64_t v)
{
    uint64_t be = htobe64(v);
    fb_bytes(fb, &be, sizeof be);
}

void fb_strn(frame_builder_t *fb, const char *s, size_t n)
{
    if (n > UINT16_MAX) {
        fb->err = true;
        return;
    }
    fb_u16(fb, (uint16_t)n);
    fb_bytes(fb, s, n);
}

void fb_str(frame_builder_t *fb, const char *s) { fb_strn(fb, s, strlen(s)); }

bool fb_finish(frame_builder_t *fb)
{
    if (fb->err)
        return false;
    uint32_t wire_len = (uint32_t)(fb->used - HEADER_SIZE);  /* type + payload */
    if (wire_len > MAX_WIRE_LEN) {
        fb->err = true;
        return false;
    }
    uint32_t be = htobe32(wire_len);
    memcpy(fb->buf, &be, sizeof be);
    return true;
}

/* ================ one helper per server->client frame ================ */

void enc_login_ok(frame_builder_t *fb, uint32_t n_unread)
{
    fb_begin(fb, T_LOGIN_OK);
    fb_u32(fb, n_unread);
    fb_finish(fb);
}

void enc_ok(frame_builder_t *fb, uint64_t ref, const char *text)
{
    fb_begin(fb, T_OK);
    fb_u64(fb, ref);
    fb_str(fb, text);
    fb_finish(fb);
}

void enc_error(frame_builder_t *fb, uint8_t code, const char *text)
{
    fb_begin(fb, T_ERROR);
    fb_u8(fb, code);
    fb_str(fb, text);
    fb_finish(fb);
}

void enc_msg(frame_builder_t *fb, uint64_t id, const char *from, const char *group,
             const char *text, uint64_t expires_at)
{
    fb_begin(fb, T_MSG);
    fb_u64(fb, id);
    fb_str(fb, from);
    fb_str(fb, group);
    fb_str(fb, text);
    fb_u64(fb, expires_at);
    fb_finish(fb);
}

void enc_history_item(frame_builder_t *fb, uint64_t id, const char *from, const char *to,
                      const char *text, uint8_t status, uint64_t created_at,
                      uint64_t expires_at)
{
    fb_begin(fb, T_HISTORY_ITEM);
    fb_u64(fb, id);
    fb_str(fb, from);
    fb_str(fb, to);
    fb_str(fb, text);
    fb_u8(fb, status);
    fb_u64(fb, created_at);
    fb_u64(fb, expires_at);
    fb_finish(fb);
}

void enc_history_end(frame_builder_t *fb, uint32_t count)
{
    fb_begin(fb, T_HISTORY_END);
    fb_u32(fb, count);
    fb_finish(fb);
}

void enc_name_list(frame_builder_t *fb, uint8_t type, char (*names)[NAME_BUF], int n)
{
    fb_begin(fb, type);
    fb_u16(fb, (uint16_t)n);
    for (int i = 0; i < n; i++)
        fb_str(fb, names[i]);
    fb_finish(fb);
}

void enc_receipt(frame_builder_t *fb, uint64_t id, const char *reader, const char *preview)
{
    fb_begin(fb, T_RECEIPT);
    fb_u64(fb, id);
    fb_str(fb, reader);
    fb_str(fb, preview);
    fb_finish(fb);
}

void enc_expired(frame_builder_t *fb, uint64_t id, const char *from)
{
    fb_begin(fb, T_EXPIRED);
    fb_u64(fb, id);
    fb_str(fb, from);
    fb_finish(fb);
}

void enc_text_frame(frame_builder_t *fb, uint8_t type, const char *text)
{
    fb_begin(fb, type);
    fb_str(fb, text);
    fb_finish(fb);
}

/* ======================= reading frames ======================= */

void reader_init(reader_t *r, const frame_t *f)
{
    r->p = f->payload;
    r->left = f->payload_len;
    r->err = false;
    r->too_long = false;
}

/* Copies n bytes out of the payload, or sets err if they are not there. */
static bool rd_bytes(reader_t *r, void *out, size_t n)
{
    if (r->err || r->left < n) {
        r->err = true;
        memset(out, 0, n);
        return false;
    }
    memcpy(out, r->p, n);
    r->p += n;
    r->left -= n;
    return true;
}

uint8_t rd_u8(reader_t *r)
{
    uint8_t v;
    rd_bytes(r, &v, 1);
    return v;
}

uint16_t rd_u16(reader_t *r)
{
    uint16_t v;
    rd_bytes(r, &v, sizeof v);
    return be16toh(v);
}

uint32_t rd_u32(reader_t *r)
{
    uint32_t v;
    rd_bytes(r, &v, sizeof v);
    return be32toh(v);
}

uint64_t rd_u64(reader_t *r)
{
    uint64_t v;
    rd_bytes(r, &v, sizeof v);
    return be64toh(v);
}

void rd_str(reader_t *r, char *out, size_t out_size)
{
    out[0] = '\0';
    uint16_t n = rd_u16(r);
    if (r->err)
        return;
    if (n >= out_size || n > r->left) {
        r->err = true;
        return;
    }
    memcpy(out, r->p, n);
    out[n] = '\0';
    r->p += n;
    r->left -= n;
    if (strlen(out) != n)      /* an embedded NUL byte is not allowed */
        r->err = true;
}

void rd_text(reader_t *r, char *out)
{
    out[0] = '\0';
    uint16_t n = rd_u16(r);
    if (r->err)
        return;
    if (n > r->left) {
        r->err = true;
        return;
    }
    if (n > MAX_TEXT) {        /* well-formed but too long: skip it */
        r->too_long = true;
        r->p += n;
        r->left -= n;
        return;
    }
    memcpy(out, r->p, n);
    out[n] = '\0';
    r->p += n;
    r->left -= n;
    if (strlen(out) != n)
        r->err = true;
}

bool rd_ok(const reader_t *r) { return !r->err && !r->too_long && r->left == 0; }

/* ======================= names ======================= */

static bool valid_name_chars(const char *s, size_t min_len, size_t max_len)
{
    size_t n = strlen(s);
    if (n < min_len || n > max_len)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_'))
            return false;
    }
    return true;
}

bool valid_username(const char *s) { return valid_name_chars(s, 1, MAX_NAME); }

bool valid_groupname(const char *s)
{
    return s[0] == '#' && valid_name_chars(s + 1, 1, MAX_NAME - 1);
}

bool is_group_name(const char *s) { return s[0] == '#'; }

void make_preview(const char *text, char *out)
{
    size_t n = strlen(text);
    if (n <= PREVIEW_MAX) {
        memcpy(out, text, n + 1);
        return;
    }
    size_t keep = PREVIEW_MAX - 3;   /* room for "..." */
    /* Do not cut in the middle of a UTF-8 character: continuation bytes
     * look like 10xxxxxx, so step back until we are at a character start. */
    while (keep > 0 && ((unsigned char)text[keep] & 0xC0) == 0x80)
        keep--;
    memcpy(out, text, keep);
    memcpy(out + keep, "...", 4);
}
